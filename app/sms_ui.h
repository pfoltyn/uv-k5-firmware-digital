/* SMS screen: compose, send, and read what arrives.
 *
 * Reached by holding 0 on the main screen, the slot POCSAG and APRS also use; all
 * three are mutually exclusive builds so there is no contention.
 *
 * The passphrase is compiled in rather than typed. It is a shared secret set once
 * per pair of radios, and multi-tap entry of a high-entropy passphrase on a numeric
 * keypad is both miserable and the single most expensive thing the screen could
 * contain. Only the message text needs alpha entry.
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

#ifndef SMS_UI_H
#define SMS_UI_H

// Takes over the display and keypad until EXIT, then hands the radio back.
void APP_RunSms(void);

#endif
