# FlowLimiter

The flow limiter system limits daily outflow from a water tank. It uses an Internet of Things (IoT)
device to monitor water flow and limit it to a daily max, and to connect to the cloud and report
flow data to the cloud. The cloud sends text-message alerts when anomalies are detected.
The cloud allows users to view historical data.

## Design Documentation

System design documentation and requirements are at: 
https://sites.google.com/site/paulbouchier/home/flow-limiter

## Certificates and Keys

This repo contains a file: secrets.template.h. Follow the instructions in that file to install
your own certificates and keys, and rename it to secrets.h to enable building the project in
the Arduino environment.

## Build

### Prerequisites

You must have the following libraries installed in your Arduino environment:
- EEPROM
- 
