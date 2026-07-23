# Automated Chessboard with Voice Interface

A mechatronic chess system that combines a CoreXY motion mechanism, an electromagnet, voice recognition, chess-rule validation, and the Stockfish chess engine to move physical chess pieces automatically.

The project was developed as a diploma thesis in the Department of Production and Management Engineering at Democritus University of Thrace.

## Project Overview

The system allows a user to control a physical chessboard using spoken commands.

A microphone captures the command, Faster-Whisper converts speech to text, and the Python application validates the requested move using `python-chess`. The move is then transmitted to an Arduino controller, which drives the CoreXY mechanism and electromagnet.

The system also supports playing against Stockfish. After the player's move, the chess engine calculates a response and the mechanism moves the corresponding physical piece.

## Main Features

- Voice-controlled chess moves
- Offline speech recognition using Faster-Whisper
- NATO phonetic alphabet support for improved square recognition
- Legal-move validation using `python-chess`
- Stockfish-based computer opponent
- Multiple difficulty levels
- CoreXY motion control
- Electromagnetic piece handling
- Safe routing between occupied chessboard squares
- Automatic handling of captured pieces
- Dedicated storage areas for captured pieces
- Serial communication using an `ACK / DONE / ERR` protocol
- Diagnostic tools for microphone and Arduino communication

## System Architecture

```text
Voice Command
      |
      v
Microphone Input
      |
      v
Faster-Whisper Speech Recognition
      |
      v
Command Parsing and Chess Validation
      |
      +-------------------+
      |                   |
      v                   v
python-chess          Stockfish Engine
      |                   |
      +---------+---------+
                |
                v
        Serial Communication
                |
                v
             Arduino
                |
                v
      CoreXY Motors + Electromagnet
                |
                v
       Physical Chess Piece Motion
```

## Repository Structure

```text
automated-chessboard/
├── arduino/
│   └── corexy_chessboard_controller.ino
│
├── src/
│   ├── arduino_bridge.py
│   ├── chess_core.py
│   └── voice_chess_vad.py
│
├── tools/
│   ├── arduino_ping_test.py
│   ├── list_ports.py
│   ├── mic_live_debug.py
│   └── stt_loop_test_v2.py
│
├── .gitignore
├── README.md
└── requirements.txt
```

## File Description

### `arduino/corexy_chessboard_controller.ino`

Arduino firmware responsible for:

- CoreXY motor control
- Coordinate and chess-square mapping
- Electromagnet activation
- Safe movement routing
- Captured-piece storage
- Serial command parsing
- Soft motion limits
- Position tracking
- Error reporting

### `src/arduino_bridge.py`

Python interface for communication with the Arduino.

It sends commands and waits for the serial protocol responses:

```text
ACK
DONE
ERR <message>
DBG <message>
```

### `src/chess_core.py`

Offline command-line chess controller that integrates:

- `python-chess`
- Stockfish
- Arduino communication
- Legal move validation
- Physical piece movement
- Move evaluation and feedback

### `src/voice_chess_vad.py`

Main voice-control application integrating:

- Faster-Whisper
- WebRTC Voice Activity Detection
- NATO phonetic alphabet parsing
- Chess move validation
- Stockfish
- Arduino motion control
- Physical capture handling
- Castling movement

### `tools/`

Contains diagnostic and development utilities:

- Arduino connection test
- Serial-port detection
- Live microphone-level monitoring
- Speech-to-text command testing

## Hardware

The prototype includes:

- Arduino-compatible microcontroller
- Two NEMA 17 stepper motors
- Stepper motor drivers
- CoreXY belt-and-pulley mechanism
- Linear guide rails
- Electromagnet
- MOSFET switching circuit
- Flyback diode
- External power supply
- Microphone
- Computer or embedded host for Python execution
- Custom physical chessboard and captured-piece storage areas

## Software

- Python 3
- Arduino IDE
- Faster-Whisper
- WebRTC VAD
- `python-chess`
- Stockfish
- PySerial
- SoundDevice
- NumPy

## Installation

Clone the repository:

```bash
git clone https://github.com/sirinopoulou/automated-chessboard.git
cd automated-chessboard
```

Create a virtual environment:

```bash
python -m venv .venv
```

Activate it on Windows:

```bash
.venv\Scripts\activate
```

Install the required Python packages:

```bash
pip install -r requirements.txt
```

## Stockfish Setup

Stockfish is not included in this repository.

Download the appropriate Stockfish executable separately and update `STOCKFISH_PATH` inside:

```text
src/chess_core.py
src/voice_chess_vad.py
```

Example:

```python
STOCKFISH_PATH = r"C:\path\to\stockfish.exe"
```

## Arduino Setup

1. Open:

```text
arduino/corexy_chessboard_controller.ino
```

2. Select the correct Arduino board and serial port.
3. Upload the firmware.
4. Confirm that the baud rate is set to:

```text
115200
```

5. Update the serial port in `src/arduino_bridge.py` when necessary:

```python
port="COM3"
```

The available ports can be displayed by running:

```bash
python tools/list_ports.py
```

## Voice Configuration

The audio input device is configured inside `src/voice_chess_vad.py`.

To identify and test the microphone, use:

```bash
python tools/mic_live_debug.py
```

The device number and sample rate may need to be changed depending on the computer and microphone.

## Running the System

From the repository root, run:

```bash
python src/voice_chess_vad.py
```

Example spoken command using the NATO alphabet:

```text
Move echo two to echo four
```

Other supported commands include:

```text
New game
Go home
Zero
Board
Mode manual
Mode vs
Level easy
Level medium
Level hard
Stop
```

## Arduino Serial Commands

The Arduino controller supports commands such as:

```text
PING
Z
H
G <x_mm> <y_mm>
R <dx_mm> <dy_mm>
SQ <square>
PMV <from> <to>
CAP <from> <to> <piece>
MAG 0
MAG 1
STOP
```

## Calibration

The following parameters are specific to the physical prototype and may require adjustment:

```cpp
const float STEPS_PER_MM = 20.13f;
const float MAX_X_MM = 460.0f;
const float MAX_Y_MM = 670.0f;
const float SQ_MM = 50.0f;
const float GAP_MM = 25.0f;
```

Motor direction, electromagnet polarity, board orientation, and coordinate offsets may also need to be adapted to a different mechanical setup.

## Current Limitations

- En passant is not physically implemented.
- Promotion requires manual replacement of the promoted piece.
- Device numbers and executable paths are currently configured locally.
- The software requires the physical hardware and calibration values used by the prototype.
- The current system tracks board state through commanded moves rather than computer vision or piece-detection sensors.

## Engineering Challenges

### Safe Piece Routing

A chess piece cannot always travel directly between two squares because other pieces may obstruct the path. The firmware therefore moves pieces through corridors between square centers before reaching the destination.

### Captured-Piece Management

Captured pieces are automatically transported to dedicated storage areas outside the main board. Pawns are assigned dynamically to available slots, while major pieces use predefined positions.

### Voice Recognition Reliability

Chess coordinates can be difficult to recognize accurately from speech. NATO phonetic words such as `alpha`, `bravo`, and `echo` are used to reduce ambiguity.

### Hardware–Software Synchronization

The Python application waits for explicit `ACK`, `DONE`, or `ERR` responses from the Arduino before continuing, preventing the chess state from advancing before the physical motion has completed.

### Embedded Memory Constraints

The Arduino firmware avoids the dynamic `String` class and stores constant serial messages in flash memory to reduce SRAM usage.

## Academic Context

This repository contains the software developed for the diploma thesis:

**“Design and Implementation of a Mechatronic System for an Automated Chessboard with Voice Interface and CoreXY Motion Mechanism.”**

The project combines mechanical design, electronics, embedded programming, motion control, artificial intelligence, and speech processing.

## Author

**Konstantina Metaxenia Sirinopoulou**

Department of Production and Management Engineering  
Democritus University of Thrace

## License

This repository is provided for educational and portfolio purposes.
