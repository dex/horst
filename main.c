/* horst - Highly Optimized Radio Scanning Tool
 *
 * Copyright (C) 2005-2011 Bruno Randolf (br1@einfach.org)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <signal.h>
#include <sys/time.h>
#include <errno.h>
#include <err.h>
#include <sys/socket.h>

#include "main.h"
#include "util.h"
#include "capture.h"
#include "protocol_parser.h"
#include "network.h"
#include "display.h"
#include "ieee80211.h"
#include "ieee80211_util.h"
#include "wext.h"


struct list_head nodes;
struct essid_meta_info essids;
struct history hist;
struct statistics stats;
struct chan_freq channels[MAX_CHANNELS];
struct channel_info spectrum[MAX_CHANNELS];

struct config conf = {
	.node_timeout		= NODE_TIMEOUT,
	.channel_time		= CHANNEL_TIME,
	.ifname			= INTERFACE_NAME,
	.display_interval	= DISPLAY_UPDATE_INTERVAL,
	.filter_pkt		= 0xffffff,
	.recv_buffer_size	= RECV_BUFFER_SIZE,
	.port			= DEFAULT_PORT,
};

struct timeval the_time;

int mon; /* monitoring socket */

static FILE* DF = NULL;

/* receive packet buffer
 *
 * due to the way we receive packets the network (TCP connection) we have to
 * expect the reception of partial packet as well as the reception of several
 * packets at one. thus we implement a buffered receive where partially received
 * data stays in the buffer.
 *
 * we need two buffers: one for packet capture or receiving from the server and
 * another one for data the clients sends to the server.
 *
 * not sure if this is also an issue with local packet capture, but it is not
 * implemented there.
 *
 * size: max 80211 frame (2312) + space for prism2 header (144)
 * or radiotap header (usually only 26) + some extra */
static unsigned char buffer[2312 + 200];
static size_t buflen;

/* for packets from client to server */
static unsigned char cli_buffer[500];
static size_t cli_buflen;

/* for select */
static fd_set read_fds;
static fd_set write_fds;
static fd_set excpt_fds;
static struct timeval tv;


struct node_info* node_update(struct packet_info* p);
void update_essids(struct packet_info* p, struct node_info* n);
void timeout_nodes(void);
int auto_change_channel(int mon);
void get_current_channel(int mon);


void __attribute__ ((format (printf, 1, 2)))
printlog(const char *fmt, ...)
{
	char buf[128];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(&buf[1], 127, fmt, ap);
	va_end(ap);

	if (conf.quiet || DO_DEBUG || !conf.display_initialized)
		printf("%s\n", &buf[1]);
	else {
		/* fix up string for display log */
		buf[0] = '\n';
		display_log(buf);
	}
}


static void
update_history(struct packet_info* p)
{
	if (p->phy_signal == 0)
		return;

	hist.signal[hist.index] = p->phy_signal;
	hist.noise[hist.index] = p->phy_noise;
	hist.rate[hist.index] = p->phy_rate;
	hist.type[hist.index] = p->wlan_type;
	hist.retry[hist.index] = p->wlan_retry;

	hist.index++;
	if (hist.index == MAX_HISTORY)
		hist.index = 0;
}


static void
update_statistics(struct packet_info* p)
{
	if (p->phy_rate == 0)
		return;

	stats.packets++;
	stats.bytes += p->wlan_len;
	if (p->wlan_retry)
		stats.retries++;

	if (p->phy_rate > 0 && p->phy_rate < MAX_RATES) {
		stats.duration += p->pkt_duration;
		stats.packets_per_rate[p->phy_rate]++;
		stats.bytes_per_rate[p->phy_rate] += p->wlan_len;
		stats.duration_per_rate[p->phy_rate] += p->pkt_duration;
	}
	if (p->wlan_type >= 0 && p->wlan_type < MAX_FSTYPE) {
		stats.packets_per_type[p->wlan_type]++;
		stats.bytes_per_type[p->wlan_type] += p->wlan_len;
		if (p->phy_rate > 0 && p->phy_rate < MAX_RATES)
			stats.duration_per_type[p->wlan_type] += p->pkt_duration;
	}
}


static void
update_spectrum(struct packet_info* p, struct node_info* n)
{
	struct channel_info* chan;
	struct chan_node* cn;

	if (p->pkt_chan_idx < 0)
		return; /* chan not found */

	chan = &spectrum[p->pkt_chan_idx];
	chan->signal = p->phy_signal;
	chan->noise = p->phy_noise;
	chan->packets++;
	chan->bytes += p->wlan_len;
	chan->durations += p->pkt_duration;
	ewma_add(&chan->signal_avg, -chan->signal);

	if (!n)
		return;

	/* add node to channel if not already there */
	list_for_each_entry(cn, &chan->nodes, chan_list) {
		if (cn->node == n) {
			DEBUG("SPEC node found %p\n", cn->node);
			break;
		}
	}
	if (cn->node != n) {
		DEBUG("SPEC node adding %p\n", n);
		cn = malloc(sizeof(struct chan_node));
		cn->node = n;
		cn->chan = chan;
		ewma_init(&cn->sig_avg, 1024, 8);
		list_add_tail(&cn->chan_list, &chan->nodes);
		list_add_tail(&cn->node_list, &n->on_channels);
		chan->num_nodes++;
		n->num_on_channels++;
	}
	/* keep signal of this node as seen on this channel */
	cn->sig = p->phy_signal;
	ewma_add(&cn->sig_avg, -cn->sig);
	cn->packets++;
}


void
update_spectrum_durations(void)
{
	/* also if channel was not changed, keep stats only for every channel_time.
	 * display code uses durations_last to get a more stable view */
	if (conf.current_channel >= 0) {
		spectrum[conf.current_channel].durations_last =
				spectrum[conf.current_channel].durations;
		spectrum[conf.current_channel].durations = 0;
		ewma_add(&spectrum[conf.current_channel].durations_avg,
			 spectrum[conf.current_channel].durations_last);
	}
}


static void 
write_to_file(struct packet_info* p)
{
	fprintf(DF, "%s, %s, ",
		get_packet_type_name(p->wlan_type), ether_sprintf(p->wlan_src));
	fprintf(DF, "%s, ", ether_sprintf(p->wlan_dst));
	fprintf(DF, "%s, ", ether_sprintf(p->wlan_bssid));
	fprintf(DF, "%x, %d, %d, %d, %d, %d, ",
		p->pkt_types, p->phy_signal, p->phy_noise, p->phy_snr,
		p->wlan_len, p->phy_rate);
	fprintf(DF, "%016llx, ", (unsigned long long)p->wlan_tsf);
	fprintf(DF, "%s, %d, %d, %d, ",
		p->wlan_essid, p->wlan_mode, p->wlan_channel, p->wlan_wep);
	fprintf(DF, "%s, ", ip_sprintf(p->ip_src));
	fprintf(DF, "%s, ", ip_sprintf(p->ip_dst));
	fprintf(DF, "%d, %d, %d\n", p->olsr_type, p->olsr_neigh, p->olsr_tc);
}


static int
filter_packet(struct packet_info* p)
{
	int i;

	if (conf.filter_off)
		return 0;

	if (!(p->pkt_types & conf.filter_pkt)) {
		stats.filtered_packets++;
		return 1;
	}

	if (MAC_NOT_EMPTY(conf.filterbssid) &&
	    memcmp(p->wlan_bssid, conf.filterbssid, MAC_LEN) != 0) {
		stats.filtered_packets++;
		return 1;
	}

	if (conf.do_macfilter) {
		for (i = 0; i < MAX_FILTERMAC; i++) {
			if (MAC_NOT_EMPTY(p->wlan_src) &&
			    conf.filtermac_enabled[i] &&
			    memcmp(p->wlan_src, conf.filtermac[i], MAC_LEN) == 0) {
				return 0;
			}
		}
		stats.filtered_packets++;
		return 1;
	}
	return 0;
}


void
handle_packet(struct packet_info* p)
{
	struct node_info* n;
	int i = -1;

	/* filter on server side only */
	if (!conf.serveraddr && filter_packet(p)) {
		if (!conf.quiet && !conf.paused)
			update_display_clock();
		return;
	}

	if (cli_fd != -1)
		net_send_packet(p);

	if (conf.dumpfile != NULL)
		write_to_file(p);

	if (conf.quiet || conf.paused)
		return;

	/* get channel index for packet */
	if (p->phy_chan) {
		/* find channel index from packet channel */
		for (i = 0; i < conf.num_channels && i < MAX_CHANNELS; i++)
			if (channels[i].chan == p->phy_chan)
				break;
	}
	/* not found from pkt, best guess from config but it might be
	 * unknown (-1) too */
#ifdef WINPCAP
	p->pkt_chan_idx = conf.current_channel = p->wlan_channel;
#else
	if (i < 0 || i >= conf.num_channels || i >= MAX_CHANNELS)
		p->pkt_chan_idx = conf.current_channel;
	else
		p->pkt_chan_idx = i;
#endif

	/* detect if noise reading is present or not */
	if (!conf.have_noise && p->phy_noise)
		conf.have_noise = 1;

	/* if current channel is unknown (this is a mac80211 bug), guess it from
	 * the packet */
	if (conf.current_channel < 0 && p->pkt_chan_idx >= 0)
		conf.current_channel = p->pkt_chan_idx;

	n = node_update(p);

	if (n)
		p->wlan_retries = n->wlan_retries_last;

	p->pkt_duration = ieee80211_frame_duration(
				p->phy_flags & PHY_FLAG_MODE_MASK,
				p->wlan_len, p->phy_rate * 5,
				p->phy_flags & PHY_FLAG_SHORTPRE,
				0 /*shortslot*/, p->wlan_type, p->wlan_qos_class,
				p->wlan_retries);

	update_history(p);
	update_statistics(p);
	update_spectrum(p, n);
	update_essids(p, n);

#if !DO_DEBUG
	update_display(p, n);
#endif
}


static void
local_receive_packet(int fd, unsigned char* buffer, size_t bufsize)
{
	int len;
	struct packet_info p;

	len = recv_packet(fd, buffer, bufsize);

#if DO_DEBUG
	dump_packet(buffer, len);
#endif
	memset(&p, 0, sizeof(p));

	if (!parse_packet(buffer, len, &p)) {
		DEBUG("parsing failed\n");
		return;
	}

	handle_packet(&p);
}


static void
receive_any(void)
{
	int ret, mfd;

	FD_ZERO(&read_fds);
	FD_ZERO(&write_fds);
	FD_ZERO(&excpt_fds);
	FD_SET(0, &read_fds);
	FD_SET(mon, &read_fds);
	if (srv_fd != -1)
		FD_SET(srv_fd, &read_fds);
	if (cli_fd != -1)
		FD_SET(cli_fd, &read_fds);

	tv.tv_sec = 0;
	tv.tv_usec = min(conf.channel_time, 1000000);
	mfd = max(mon, srv_fd);
	mfd = max(mfd, cli_fd) + 1;

	ret = select(mfd, &read_fds, &write_fds, &excpt_fds, &tv);
	if (ret == -1 && errno == EINTR) /* interrupted */
		return;
	if (ret == 0) { /* timeout */
		if (!conf.quiet && !DO_DEBUG)
			update_display_clock();
		return;
	}
	else if (ret < 0) /* error */
		err(1, "select()");

	/* stdin */
	if (FD_ISSET(0, &read_fds) && !conf.quiet)
		handle_user_input();

	/* local packet or client */
	if (FD_ISSET(mon, &read_fds)) {
		if (conf.serveraddr)
			net_receive(mon, buffer, &buflen, sizeof(buffer));
		else
			local_receive_packet(mon, buffer, sizeof(buffer));
	}

	/* server */
	if (srv_fd > -1 && FD_ISSET(srv_fd, &read_fds))
		net_handle_server_conn();

	/* from client to server */
	if (cli_fd > -1 && FD_ISSET(cli_fd, &read_fds))
		net_receive(cli_fd, cli_buffer, &cli_buflen, sizeof(cli_buffer));
}


void
free_lists(void)
{
	int i;
	struct essid_info *e, *f;
	struct node_info *ni, *mi;
	struct chan_node *cn, *cn2;

	/* free node list */
	list_for_each_entry_safe(ni, mi, &nodes, list) {
		DEBUG("free node %s\n", ether_sprintf(ni->last_pkt.wlan_src));
		list_del(&ni->list);
		free(ni);
	}

	/* free essids */
	list_for_each_entry_safe(e, f, &essids.list, list) {
		DEBUG("free essid '%s'\n", e->essid);
		list_del(&e->list);
		free(e);
	}

	/* free channel nodes */
	for (i = 0; i < conf.num_channels; i++) {
		list_for_each_entry_safe(cn, cn2, &spectrum[i].nodes, chan_list) {
			DEBUG("free chan_node %p\n", cn);
			list_del(&cn->chan_list);
			cn->chan->num_nodes--;
			free(cn);
		}
	}
}


static void
finish_all(void)
{
	free_lists();

	if (!conf.serveraddr)
		close_packet_socket(mon, conf.ifname);

	if (DF != NULL)
		fclose(DF);

#if !DO_DEBUG
	net_finish();

	if (!conf.quiet)
		finish_display();
#endif
}


static void
exit_handler(void)
{
	finish_all();
}


static void
sigint_handler(int sig)
{
	exit(0);
}


static void
sigpipe_handler(int sig)
{
	/* ignore signal here - we will handle it after write failed */
}


static void
get_options(int argc, char** argv)
{
	int c;
	static int n;

	while((c = getopt(argc, argv, "hqsCi:t:c:p:e:d:o:b:")) > 0) {
		switch (c) {
		case 'p':
			conf.port = optarg;
			break;
		case 'q':
			conf.quiet = 1;
			break;
		case 'i':
			conf.ifname = optarg;
			break;
		case 'o':
			conf.dumpfile = optarg;
			break;
		case 't':
			conf.node_timeout = atoi(optarg);
			break;
		case 'b':
			conf.recv_buffer_size = atoi(optarg);
			break;
		case 's':
			conf.do_change_channel = 1;
			break;
		case 'd':
			conf.display_interval = atoi(optarg) * 1000;
			break;
		case 'e':
			if (n >= MAX_FILTERMAC)
				break;
			conf.do_macfilter = 1;
			convert_string_to_mac(optarg, conf.filtermac[n]);
			conf.filtermac_enabled[n] = 1;
			n++;
			break;
		case 'c':
			conf.serveraddr = optarg;
			break;
		case 'C':
			conf.allow_client = 1;
			break;
		case 'h':
		default:
			printf("usage: %s [-h] [-q] [-s] [-i interface] [-t sec] [-c IP] [-C] [-p port] [-e mac] [-d ms] [-o file] [-b bytes]\n\n"
				"Options (default value)\n"
				"  -h\t\tthis help\n"
				"  -q\t\tquiet\n"
				"  -s\t\tspectrum analyzer\n"
				"  -i <intf>\tinterface (wlan0)\n"
				"  -t <sec>\tnode timeout (60)\n"
				"  -c <IP>\tconnect to server\n"
				"  -C allow client connection (server)\n"
				"  -p <port>\tuse port (4444)\n"
				"  -e <mac>\tfilter all macs except these (multiple)\n"
				"  -d <ms>\tdisplay update interval (100)\n"
				"  -o <filename>\twrite packet info into file\n"
				"  -b <bytes>\treceive buffer size (not set)\n"
				"\n",
				argv[0]);
			exit(0);
			break;
		}
	}
}


int
main(int argc, char** argv)
{
	INIT_LIST_HEAD(&essids.list);
	INIT_LIST_HEAD(&nodes);

	get_options(argc, argv);

	signal(SIGINT, sigint_handler);
	signal(SIGPIPE, sigpipe_handler);
	atexit(exit_handler);

	gettimeofday(&stats.stats_time, NULL);
	gettimeofday(&the_time, NULL);

	conf.current_channel = -1;

	if (conf.serveraddr)
		mon = net_open_client_socket(conf.serveraddr, conf.port);
	else {
		mon = open_packet_socket(conf.ifname, sizeof(buffer), conf.recv_buffer_size);
		if (mon <= 0)
			err(1, "Couldn't open packet socket");

		conf.arphrd = device_get_arptype(mon, conf.ifname);
		if (conf.arphrd != ARPHRD_IEEE80211_PRISM &&
		conf.arphrd != ARPHRD_IEEE80211_RADIOTAP &&
		conf.arphrd != ARPHRD_IEEE80211_PPI) {
			printf("Wrong monitor type! Please use radiotap or prism2 or ppi headers\n");
			exit(1);
		}

#ifdef WINPCAP
		conf.num_channels = wext_get_2_4_channels(channels);
		init_channels();
		conf.current_channel = 1;
#else
		/* get available channels */
		conf.num_channels = wext_get_channels(mon, conf.ifname, channels);
		init_channels();
		get_current_channel(mon);
#endif
	}

	if (!conf.quiet && !DO_DEBUG)
		init_display();

	if (conf.dumpfile != NULL) {
		DF = fopen(conf.dumpfile, "w");
		if (DF == NULL)
			err(1, "Couldn't open dump file");
	}

	if (!conf.serveraddr && conf.port && conf.allow_client)
		net_init_server_socket(conf.port);

	for ( /* ever */ ;;)
	{
		receive_any();
		gettimeofday(&the_time, NULL);
		timeout_nodes();
		if (!conf.serveraddr) { /* server */
			if (auto_change_channel(mon)) {
				net_send_channel_config();
				update_spectrum_durations();
				if (!DO_DEBUG)
					update_display(NULL, NULL);
			}
		}
	}
	/* will never */
	return 0;
}


#if 0
void print_rate_duration_table(void)
{
	int i;

	printf("LEN\t1M l\t1M s\t2M l\t2M s\t5.5M l\t5.5M s\t11M l\t11M s\t");
	printf("6M\t9\t12M\t18M\t24M\t36M\t48M\t54M\n");
	for (i=10; i<=2304; i+=10) {
		printf("%d:\t%d\t%d\t", i,
			ieee80211_frame_duration(PHY_FLAG_G, i, 10, 0, 0, IEEE80211_FTYPE_DATA, 0, 0),
			ieee80211_frame_duration(PHY_FLAG_G, i, 10, 1, 0, IEEE80211_FTYPE_DATA, 0, 0));
		printf("%d\t%d\t",
			ieee80211_frame_duration(PHY_FLAG_G, i, 20, 0, 0, IEEE80211_FTYPE_DATA, 0, 0),
			ieee80211_frame_duration(PHY_FLAG_G, i, 20, 1, 0, IEEE80211_FTYPE_DATA, 0, 0));
		printf("%d\t%d\t",
			ieee80211_frame_duration(PHY_FLAG_G, i, 55, 0, 0, IEEE80211_FTYPE_DATA, 0, 0),
			ieee80211_frame_duration(PHY_FLAG_G, i, 55, 1, 0, IEEE80211_FTYPE_DATA, 0, 0));
		printf("%d\t%d\t",
			ieee80211_frame_duration(PHY_FLAG_G, i, 110, 0, 0, IEEE80211_FTYPE_DATA, 0, 0),
			ieee80211_frame_duration(PHY_FLAG_G, i, 110, 1, 0, IEEE80211_FTYPE_DATA, 0, 0));

		printf("%d\t",
			ieee80211_frame_duration(PHY_FLAG_G, i, 60, 1, 0, IEEE80211_FTYPE_DATA, 0, 0));
		printf("%d\t",
			ieee80211_frame_duration(PHY_FLAG_G, i, 90, 1, 0, IEEE80211_FTYPE_DATA, 0, 0));
		printf("%d\t",
			ieee80211_frame_duration(PHY_FLAG_G, i, 120, 1, 0, IEEE80211_FTYPE_DATA, 0, 0)),
		printf("%d\t",
			ieee80211_frame_duration(PHY_FLAG_G, i, 180, 1, 0, IEEE80211_FTYPE_DATA, 0, 0)),
		printf("%d\t",
			ieee80211_frame_duration(PHY_FLAG_G, i, 240, 1, 0, IEEE80211_FTYPE_DATA, 0, 0)),
		printf("%d\t",
			ieee80211_frame_duration(PHY_FLAG_G, i, 360, 1, 0, IEEE80211_FTYPE_DATA, 0, 0));
		printf("%d\t",
			ieee80211_frame_duration(PHY_FLAG_G, i, 480, 1, 0, IEEE80211_FTYPE_DATA, 0, 0)),
		printf("%d\n",
			ieee80211_frame_duration(PHY_FLAG_G, i, 540, 1, 0, IEEE80211_FTYPE_DATA, 0, 0));
	}
}
#endif
