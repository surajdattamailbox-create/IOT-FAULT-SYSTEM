# Smart IoT Electrical Fault Detection System — V3.0

## Included upgrades
- Responsive local ESP32 dashboard with live voltage/current/power/energy cards.
- Live trend graph (last 60 samples) for output voltage and current.
- Password-protected admin routes using HTTP Basic Authentication.
- Adjustable voltage/current thresholds and sensor calibration, saved in ESP32 Preferences.
- Software-only fault simulation for low/high voltage, overload, suspected open circuit, and suspected short circuit.
- Recent event history and CSV export (RAM-only; last 12 events).
- Energy counter reset, ESP32 restart control, LCD startup/live pages, LED/buzzer indicators.
- Short confirmation interval for physical fault status changes to reduce rapid status flicker.

## Admin credentials
Username: `admin`  
Password: `Suraj.bh.com`

## Wi-Fi and dashboard
- SSID: `SmartFault-Setup`
- Password: `DCmonitor123`
- Dashboard: `http://192.168.4.1`

Connect your phone to the ESP32 Wi-Fi access point and open the dashboard address. If the phone says the network has no internet, choose to stay connected.

## Arduino IDE
1. Install Arduino IDE and the ESP32 board package.
2. Install `LiquidCrystal_I2C` library.
3. Open `Smart_IoT_Fault_System_V3.ino` and select the ESP32 Dev Module board.
4. Confirm the LCD I2C address (`0x27`, sometimes `0x3F`) and wiring before upload.
5. Upload and connect to the Wi-Fi details above.

## Hardware/pin plan
- GPIO32: ACS712 OUT through a suitable divider/signal conditioning.
- GPIO34: input-voltage sensor S through divider.
- GPIO35: output-voltage sensor S through divider.
- GPIO21 SDA / GPIO22 SCL: LCD via a suitable bidirectional I2C level shifter if the LCD backpack pulls the bus to 5V.
- GPIO16 green LED, GPIO17 yellow LED, GPIO23 red LED, GPIO27 blue LED, GPIO14 white LED; each LED uses a 220 ohm series resistor.
- GPIO25: buzzer driver transistor input (never drive a high-current/12V buzzer directly from ESP32).
- GPIO26: relay input, kept OFF; relay contacts are not used for automatic protection.
- All grounds in the low-voltage circuit share a common reference as appropriate for the module design.

## Important calibration notes
Default calibration values are only starting values. Compare both voltage readings with a multimeter and adjust calibration in Admin. ACS712 zero offset and sensitivity vary by board and supply; calibrate current using a known safe DC load. ACS712 20A modules are not very precise at low currents.

## Safety and limitations
- **12V DC isolated demonstration only. Never connect to 230V AC mains, phase, or neutral.**
- Never deliberately short the supply. Use software simulation for short-circuit demonstrations.
- Use a properly selected DC fuse, correctly rated wires, and a small fused test load. A holder marked 32A does not mean a 32A fuse is suitable.
- ESP32 GPIO/ADC inputs are not 5V tolerant. Verify sensor outputs and divider values with a multimeter before connecting.
- Use a logic-level shifter if the LCD I2C pull-ups are 5V.
- Fault labels are indicative only, not certified protection. A single current sensor measures total load current and cannot identify an individual parallel branch. Power factor is not measured in this DC project.
- Event history and trend samples are held in RAM and are lost after reboot. Threshold/calibration settings persist.
- Admin password is embedded in firmware and HTTP Basic Authentication is not encrypted. Use only on a trusted local demo network; do not expose the device to the internet. Change credentials in source before sharing firmware.
- Relay remains OFF and must not be used as a safety disconnect.

## Team
Soupitk Santra, Suraj Datta, Prabhas Bhunia, Devkrishna Kanta Samai, Surajit Samai.
