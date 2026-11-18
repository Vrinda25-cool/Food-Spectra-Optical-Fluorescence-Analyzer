# Food-Spectra-Optical-Fluorescence-Analyzer
Portable, non-destructive optical sensing system using ESP32 and an AS7343 14-channel spectral sensor to detect Metanil Yellow and lead chromate adulteration in ground turmeric via UV fluorescence and spectral reflectance.
## Key Features
* **Rapid & Non-Destructive:** Delivers real-time sample classification in `< 1.5 seconds` without damaging the sample.
* **Multi-Spectral Analysis:** Captures 14-channel optical signatures across visible and near-infrared wavelengths to differentiate pure turmeric from adulterated blends.
* **Light-Insulated Enclosure:** Custom 3D-printed sensing chamber eliminates ambient light interference for consistent field measurements.
* **Standalone & Portable:** Battery-powered with onboard display and physical controls for field deployment across agricultural supply chains.

## Hardware Architecture
* **Microcontroller:** ESP32
* **Optical Sensor:** AS7343 14-Channel Multi-Spectral Sensor
* **Excitation Source:** UV / Fluorescence LED illumination
* **User Interface:** OLED Display & Tactile Push-Button Controls
* **Power Supply:** 18650 Li-ion Battery Pack
* **Enclosure:** Custom 3D-Printed Light-Tight Sensing Chamber

## How It Works
1. **Sample Placement:** A ground turmeric sample is placed inside the light-isolated 3D-printed chamber.
2. **Optical Excitation:** The UV/fluorescence source illuminates the sample.
3. **Spectral Capture:** The AS7343 sensor reads the 14-channel reflectance and fluorescence response.
4. **Classification:** The ESP32 processes the spectral dataset and outputs the purity status directly to the OLED screen.
