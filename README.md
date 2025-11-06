# Fluid NC status indicator code
This code parses basic GRBL status messages and changes color of the Neopixel style LED. 
Code is written fot the Attiny 412 procesor. These pins are used:
- PA0 - only for UPDI programing, not in runtime
- PA1 - UART TX (only sending status requests "?\\n" to the GRBL machine)
- PA2 - UART RX - input pin for GRBL status messages
- PA3 - Neopixel LED data output
  
