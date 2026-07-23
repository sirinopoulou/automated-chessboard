import re
import collections
import numpy as np
import chess
import chess.engine
from faster_whisper import WhisperModel
import sounddevice as sd
import soundfile as sf
from arduino_bridge import ArduinoBridge
import webrtcvad

# -------- CONFIG --------
DEVICE = 6
SAMPLE_RATE = 48000
FILENAME = "speech_test.wav"

# VAD settings
VAD_AGGRESSIVENESS = 3      # 0–3: όσο μεγαλύτερο, τόσο πιο αυστηρό
FRAME_DURATION_MS = 30      # 10, 20 ή 30ms
PADDING_DURATION_MS = 800   # ms σιωπής για να σταματήσει η ηχογράφηση
MIN_SPEECH_DURATION_MS = 300  # ελάχιστη ομιλία για να μην αγνοηθεί ως θόρυβος

STOCKFISH_PATH = r"C:\Users\e-mashine\Desktop\nuc_voice\stockfish\stockfish-windows-x86-64-avx2\stockfish\stockfish-windows-x86-64-avx2.exe"

# -------- NATO --------
NATO_MAP = {
    "alpha": "a",
    "bravo": "b",
    "charlie": "c",
    "delta": "d",
    "echo": "e",
    "foxtrot": "f",
    "golf": "g",
    "hotel": "h",
}

NUMBER_MAP = {
    "one": "1", "two": "2", "three": "3", "four": "4",
    "five": "5", "six": "6", "seven": "7", "eight": "8"
}

# -------- INIT --------
print("Initializing voice system, please wait...")
print("Loading Whisper model (this may take a while)...")

model = WhisperModel("small", device="cpu", compute_type="int8")

print("Whisper loaded successfully.")

print("Starting Chess Engine...")
engine = chess.engine.SimpleEngine.popen_uci(STOCKFISH_PATH)

board = chess.Board()
bridge = ArduinoBridge()
bridge.open()
bridge.ping()

print("Initializing VAD...")
vad = webrtcvad.Vad(VAD_AGGRESSIVENESS)

print("=" * 50)
print("READY (NATO mode) – Continuous Listening")
print("Example: move bravo seven to bravo six")
print("Say 'stop' to exit.")
print("=" * 50)


# -------- VAD RECORDING --------

def record_until_silence():
    frame_size = int(SAMPLE_RATE * FRAME_DURATION_MS / 1000)
    num_padding_frames = int(PADDING_DURATION_MS / FRAME_DURATION_MS)
    min_speech_frames = int(MIN_SPEECH_DURATION_MS / FRAME_DURATION_MS)

    ring_buffer = collections.deque(maxlen=num_padding_frames)
    voiced_frames = []
    triggered = False
    speech_frame_count = 0

    print("\n  Listening...", flush=True)

    with sd.InputStream(
        samplerate=SAMPLE_RATE,
        channels=1,
        dtype="int16",
        device=DEVICE
    ) as stream:
        while True:
            audio_chunk, _ = stream.read(frame_size)
            frame_bytes = audio_chunk.tobytes()

            expected_bytes = frame_size * 2
            if len(frame_bytes) != expected_bytes:
                continue

            try:
                is_speech = vad.is_speech(frame_bytes, SAMPLE_RATE)
            except Exception:
                is_speech = False

            amplitude = np.abs(audio_chunk).mean()

            print(
                f"  [DEBUG] is_speech={is_speech}  amplitude={amplitude:.0f}    ",
                end="\r",
                flush=True
            )

            if not triggered:
                ring_buffer.append((frame_bytes, is_speech))
                num_voiced = sum(1 for _, s in ring_buffer if s)

                if num_voiced > 0.5 * ring_buffer.maxlen:
                    triggered = True
                    speech_frame_count = num_voiced
                    print("\n Recording...                              ", flush=True)
                    voiced_frames.extend(f for f, _ in ring_buffer)
                    ring_buffer.clear()
            else:
                voiced_frames.append(frame_bytes)
                ring_buffer.append((frame_bytes, is_speech))

                if is_speech:
                    speech_frame_count += 1

                num_unvoiced = sum(1 for _, s in ring_buffer if not s)

                if num_unvoiced > 0.90 * ring_buffer.maxlen:
                    if speech_frame_count < min_speech_frames:
                        print("\n  (too short, ignoring)", flush=True)
                        triggered = False
                        speech_frame_count = 0
                        voiced_frames.clear()
                        ring_buffer.clear()
                        print("  Listening...", flush=True)
                        continue

                    print("\n  Done.", flush=True)
                    break

    audio_bytes = b"".join(voiced_frames)
    audio_np = np.frombuffer(audio_bytes, dtype=np.int16)
    return audio_np


# -------- HELPERS --------

def normalize_text(text):
    s = (text or "").lower()
    s = re.sub(r"[^a-z0-9\s]", " ", s)
    words = s.split()

    converted = []
    i = 0

    while i < len(words):
        w = words[i]

        # NATO letters
        if w in NATO_MAP:
            converted.append(NATO_MAP[w])

        # Numbers
        elif w in NUMBER_MAP:
            converted.append(NUMBER_MAP[w])

        elif w == "to":
            if i > 0 and words[i - 1] in NATO_MAP:
                converted.append("2")
            else:
                converted.append(w)

        # Common speech errors for numbers
        elif w == "for":
            converted.append("4")

        elif w == "tree":
            converted.append("3")

        elif w == "ate":
            converted.append("8")

        else:
            converted.append(w)

        i += 1

    return " ".join(converted)


def parse_command(text):
    s = normalize_text(text)

    if "stop" in s:
        return "STOP"

    if s in ("new game", "new"):
        return "NEW"

    if "origin" in s or "home" in s:
        return "ORIGIN"

    if s in ("zero", "start", "start now"):
        return "ZERO"

    if "board" in s or "show" in s:
        return "BOARD"

    if s == "level easy":
        return ("LEVEL", "easy")

    if s == "level medium":
        return ("LEVEL", "medium")

    if s == "level hard":
        return ("LEVEL", "hard")

    if s in ("mode vs", "play against computer"):
        return ("MODE", "vs")

    if s == "mode manual":
        return ("MODE", "manual")

    # Move parsing
    words = s.split()

    cleaned = []
    for w in words:
        if w not in ("move", "from", "to"):
            cleaned.append(w)

    squares = []
    i = 0
    while i < len(cleaned) - 1:
        if cleaned[i] in "abcdefgh" and cleaned[i + 1] in "12345678":
            squares.append(cleaned[i] + cleaned[i + 1])
            i += 2
        else:
            i += 1

    if len(squares) >= 2:
        return ("MOVE", squares[0], squares[1])

    if len(cleaned) >= 2:
        if len(cleaned[0]) == 2 and len(cleaned[1]) == 2:
            return ("MOVE", cleaned[0], cleaned[1])

    return None


def piece_to_token(piece: chess.Piece):
    piece_symbol = piece.symbol()

    if piece_symbol.lower() == "p":
        return "bP" if piece_symbol.islower() else "wP"
    if piece_symbol.lower() == "r":
        return "bR" if piece_symbol.islower() else "wR"
    if piece_symbol.lower() == "n":
        return "bN" if piece_symbol.islower() else "wN"
    if piece_symbol.lower() == "b":
        return "bB" if piece_symbol.islower() else "wB"
    if piece_symbol.lower() == "q":
        return "bQ" if piece_symbol.islower() else "wQ"
    if piece_symbol.lower() == "k":
        return "bK" if piece_symbol.islower() else "wK"

    return None


def execute_physical_move(board: chess.Board, bridge: ArduinoBridge, mv: chess.Move):
    from_sq = chess.square_name(mv.from_square).upper()
    to_sq = chess.square_name(mv.to_square).upper()

    is_capture = board.is_capture(mv)
    is_en_passant = board.is_en_passant(mv)
    is_castling = board.is_castling(mv)

    print(f"[DEBUG] move={mv.uci()} capture={is_capture} ep={is_en_passant} castle={is_castling}")

    if is_en_passant:
        raise RuntimeError("En passant not supported physically")

    if is_castling:
        print(f"[DEBUG] Handling castling {mv.uci()}")

        if mv.uci() == "e1g1":       # White king side
            bridge.move_piece("E1", "G1")
            bridge.move_piece("H1", "F1")
            return

        elif mv.uci() == "e1c1":     # White queen side
            bridge.move_piece("E1", "C1")
            bridge.move_piece("A1", "D1")
            return

        elif mv.uci() == "e8g8":     # Black king side
            bridge.move_piece("E8", "G8")
            bridge.move_piece("H8", "F8")
            return

        elif mv.uci() == "e8c8":     # Black queen side
            bridge.move_piece("E8", "C8")
            bridge.move_piece("A8", "D8")
            return

        else:
            raise RuntimeError(f"Unknown castling move: {mv.uci()}")

    if is_capture:
        captured_piece = board.piece_at(mv.to_square)
        if captured_piece is None:
            raise RuntimeError("Capture detected but no piece on target square")

        piece_token = piece_to_token(captured_piece)
        if piece_token is None:
            raise RuntimeError("Unknown captured piece token")

        print(f"[DEBUG] Sending CAP {from_sq} {to_sq} {piece_token}")
        bridge.capture_piece(from_sq, to_sq, piece_token)
    else:
        print(f"[DEBUG] Sending PMV {from_sq} {to_sq}")
        bridge.move_piece(from_sq, to_sq)


# -------- MAIN LOOP --------

mode = "manual"
level = "easy"

try:
    while True:
        audio = record_until_silence()

        if len(audio) == 0:
            print("[WARN] Empty audio, skipping.")
            continue

        sf.write(FILENAME, audio, SAMPLE_RATE)

        segments, _ = model.transcribe(
            FILENAME,
            language="en",
            beam_size=5,
            vad_filter=True,
            initial_prompt=(
                "Chess commands using NATO alphabet: "
                "alpha bravo charlie delta echo foxtrot golf hotel. "
                "Example: move bravo seven to bravo six."
            )
        )

        raw = " ".join([seg.text for seg in segments]).strip()
        print(f"RAW: {raw}")

        if not raw:
            print("[WARN] No speech detected, skipping.")
            continue

        cmd = parse_command(raw)
        print(f"CMD: {cmd}")

        if cmd == "STOP":
            print("Stopping...")
            break

        elif cmd == "NEW":
            board.reset()
            print("[OK] New game")

        elif cmd == "ORIGIN":
            bridge.go_xy(0, 0)
            print("[OK] Going to origin")

        elif cmd == "ZERO":
            bridge.set_zero()
            print("[OK] Zero set")

        elif cmd == "BOARD":
            print(board)

        elif isinstance(cmd, tuple):

            if cmd[0] == "LEVEL":
                level = cmd[1]
                print(f"[OK] Level: {level}")

            elif cmd[0] == "MODE":
                mode = cmd[1]
                print(f"[OK] Mode: {mode}")

            elif cmd[0] == "MOVE":
                move_str = cmd[1] + cmd[2]

                try:
                    mv = chess.Move.from_uci(move_str)

                    if mv not in board.legal_moves:
                        print("[ERR] Illegal move")
                        continue

                    execute_physical_move(board, bridge, mv)
                    board.push(mv)
                    print(f"[OK] Move: {move_str}")

                    if mode == "vs":
                        time_limit = {"easy": 0.1, "medium": 0.5, "hard": 2.0}.get(level, 0.3)

                        info_list = engine.analyse(
                            board,
                            chess.engine.Limit(time=time_limit),
                            multipv=5
                        )

                        cpu = None
                        for info in info_list:
                            pv = info.get("pv", [])
                            if not pv:
                                continue

                            candidate = pv[0]

                            if board.is_en_passant(candidate):
                                print(f"[WARN] Skipping unsupported en passant: {candidate.uci()}")
                                continue

                            cpu = candidate
                            break

                        if cpu is None:
                            print("[ERR] Engine returned no supported move")
                            continue

                        execute_physical_move(board, bridge, cpu)
                        board.push(cpu)
                        print(f"[CPU] {cpu.uci()}")

                except Exception as e:
                    print(f"[ERR] {e}")

        else:
            print("[WARN] Command not recognized.")

except KeyboardInterrupt:
    print("\n[INFO] Interrupted by user (Ctrl+C).")

finally:
    print("Bye")
    engine.quit()
    bridge.close()