/* APRS payloads: the text that goes in an AX.25 UI frame's information field.
 *
 * The radio has no GPS, so a position report is built from an operator entered
 * Maidenhead locator. That is the normal arrangement for a fixed station and it
 * keeps the whole conversion in integers: the MCU has no FPU, and software
 * floating point would cost more flash than the feature.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 *     Unless required by applicable law or agreed to in writing, software
 *     distributed under the License is distributed on an "AS IS" BASIS,
 *     WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *     See the License for the specific language governing permissions and
 *     limitations under the License.
 */

#ifndef APRS_H
#define APRS_H

#include <stdbool.h>
#include <stdint.h>

#include "app/ax25.h"

/* Destination callsign. APZ is the prefix reserved for experimental software,
 * so this needs no registration; if the firmware ever gets a permanent tocall
 * it changes here and nowhere else. */
#define APRS_TOCALL          "APZK5F"

// APRS data type identifiers, the first character of the information field.
#define APRS_DTI_POSITION    '='   // no timestamp, station is messaging capable
#define APRS_DTI_STATUS      '>'
#define APRS_DTI_MESSAGE     ':'

#define APRS_ADDRESSEE_CHARS 9     // fixed width, space padded
#define APRS_MAX_MSG_TEXT    67
#define APRS_MAX_STATUS      62
#define APRS_MAX_COMMENT     43
#define APRS_MAX_SEQ         5

// Default symbol: primary table, house. A fixed locator is a home station, so
// claiming to be a moving handheld would be a small lie to the map.
#define APRS_SYM_TABLE       '/'
#define APRS_SYM_CODE        '-'

#define APRS_GRID_CHARS      6

// Latitude and longitude in hundredths of a minute, which is exactly the
// resolution APRS transmits, so no rounding happens after this point.
#define APRS_HMIN_PER_DEGREE 6000

// Parses a 4 or 6 character Maidenhead locator and gives the centre of the
// square. The subsquare may be upper or lower case; everything else must be in
// range, and out of range characters are rejected rather than clamped.
bool APRS_GridToLatLon(const char *grid, int32_t *lat_hmin, int32_t *lon_hmin);

// Replaces characters APRS reserves or cannot carry. Returns the length written.
// { } | ~ delimit message sequences and telemetry, and a CR or LF would end the
// payload early in most parsers, so all of them become '.' rather than being
// dropped: the text stays the length the operator typed.
uint32_t APRS_Sanitise(const char *text, char *out, uint32_t max);

// "=DDMM.hhN/DDDMM.hhW-comment"
uint32_t APRS_FormatPosition(char *out, uint32_t max, const char *grid,
                             char sym_table, char sym_code, const char *comment);

// ">text"
uint32_t APRS_FormatStatus(char *out, uint32_t max, const char *text);

// ":ADDRESSEE:text" with an optional "{nn" sequence number, which is what asks
// the far end for an acknowledgement. seq of 0 omits it.
uint32_t APRS_FormatMessage(char *out, uint32_t max, const AX25_Addr_t *to,
                            const char *text, uint16_t seq);

// ":ADDRESSEE:ackNN", the reply to a message that carried a sequence number.
uint32_t APRS_FormatAck(char *out, uint32_t max, const AX25_Addr_t *to,
                        const char *seq);

// Splits a received message payload. to, text and seq may each be NULL if not
// wanted. seq comes back empty when the sender asked for no acknowledgement.
bool APRS_ParseMessage(const char *info, AX25_Addr_t *to,
                       char *text, uint32_t text_max,
                       char *seq, uint32_t seq_max);

#endif
