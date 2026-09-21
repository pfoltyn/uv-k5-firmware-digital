/* APRS screen.
 *
 * Reached by holding 0 on the main screen, the same slot POCSAG uses, which is
 * free in the default build because it only does anything when the FM radio is
 * compiled in. The two features are mutually exclusive builds, so there is no
 * contention.
 *
 * Deliberately numeric only. Alpha entry for callsigns, paths and comments is
 * where most of a full APRS screen's flash goes, and none of it is needed to get
 * the transmitter measured, which is what this phase is for. The callsign and
 * path come from the build (APRS_UI_CALL, APRS_UI_PATH, APRS_UI_GRID) and the
 * screen edits only what a measurement needs.
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

#ifndef APRS_UI_H
#define APRS_UI_H

// Takes over the display and keypad until the user presses EXIT, then hands the
// radio back to the main screen.
void APP_RunAprs(void);

#endif
