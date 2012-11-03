#ifndef _PPI_HEADER_H
#define _PPI_HEADER_H 

#include <sys/types.h>

typedef struct ppi_packetheader {
    u_int8_t pph_version; /* Version. Currently 0 */
    u_int8_t pph_flags; /* Flags.  */
    u_int16_t pph_len; /* Length of entire message, including this header and TLV payload. */
    u_int32_t pph_dlt; /* Data Link Type of the captured packet data. */
} ppi_packetheader_t;

typedef struct ppi_fieldheader {
    u_int16_t pfh_type; /* Type */
    u_int16_t pfh_datalen; /* Length of data */
} ppi_fieldheader_t;

#define PFH_ALIGN_LEN(pfh) ((le16toh((pfh)->pfh_datalen) + 3) & ~3)
#define PFH_DATA_LEN(pph, pfh) ((pph)->pph_flags & 0x1 ? PFH_ALIGN_LEN(pfh) : le16toh((pfh)->pfh_datalen))  

#define PFH_TYPE_802_11_COMMON      2
#define PFH_TYPE_802_11N_MAC        3
#define PFH_TYPE_802_11N_MAC_PHY    4

typedef struct ppi_field80211com {
    u_int64_t com_tsf;
    u_int16_t com_flags;
    u_int16_t com_rate;
    u_int16_t com_chnl_freq;
    u_int16_t com_chnl_flags;
    u_int8_t com_fhss_hopset;
    u_int8_t com_fhss_pattern;
    u_int8_t com_signal;
    u_int8_t com_noise;
} ppi_field80211com_t;

#define COM_FLAG_BADFCS (1<<2)

#define COM_CHNL_FLAG_CCK   (1<<5)
#define COM_CHNL_FLAG_OFDM  (1<<6)
#define COM_CHNL_FLAG_2GHZ  (1<<7)
#define COM_CHNL_FLAG_5GHZ  (1<<8)

#endif  
