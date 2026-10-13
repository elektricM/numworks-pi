#ifndef NWPI_PROTOCOL_H
#define NWPI_PROTOCOL_H

/*
 * NWPI Protocol v1.0
 *
 * Message format: CMD:PAYLOAD\n
 * Legacy format:  :HEXDATA\n  (treated as KEY:HEXDATA)
 *
 * Commands:
 *   KEY  - Keyboard state (64-bit hex bitmap)
 *   AI   - AI text query
 *   AIV  - AI vision query (auto-snap + query)
 *   AIA  - AI query with last photo
 *   AIR  - AI response
 *   AIS  - AI streaming chunk
 *   AIE  - AI stream end
 *   CAM  - Camera control
 *   SYS  - System control (calc -> Pi "SYS:DFU": calc entered USB DFU,
 *          the Pi powers off before the flash ends with a reset)
 *   DSP  - Display bridge: calc "DSP:2" = speaks protocol 2; Pi "DSP:ON",
 *          "DSP:OFF" (calc answers "DSP:OK"), "DSP:STAT", "DSP:PX:x,y"
 *   OK   - Acknowledgment
 *   ERR  - Error
 */

#define NWPI_MAX_PAYLOAD  1024
#define NWPI_MAX_MSG      (NWPI_MAX_PAYLOAD + 8)  /* CMD: + payload + \n */
#define NWPI_MAX_CMD      4

/* Command strings */
#define CMD_KEY   "KEY"
#define CMD_AI    "AI"
#define CMD_AIV   "AIV"
#define CMD_AIA   "AIA"
#define CMD_AIR   "AIR"
#define CMD_AIS   "AIS"
#define CMD_AIE   "AIE"
#define CMD_CAM   "CAM"
#define CMD_SYS   "SYS"
#define CMD_OK    "OK"
#define CMD_ERR   "ERR"
#define CMD_MODE  "MODE"
#define CMD_DSP   "DSP"

#endif /* NWPI_PROTOCOL_H */
