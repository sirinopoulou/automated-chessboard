import re
from faster_whisper import WhisperModel
import sounddevice as sd
import soundfile as sf

DEVICE = 4
SAMPLERATE = 16000
SECONDS = 5
FILENAME = "speech_test.wav"

SQUARE_RE = re.compile(r"\b([a-h])\s*([1-8])\b", re.IGNORECASE)

def normalize_text(text: str) -> str:
    s = (text or "").strip().lower()

    # punctuation -> spaces
    s = re.sub(r"[^a-z0-9\s]", " ", s)

    # common speech mistakes / alternatives
    s = s.replace("go origin", "go to origin")
    s = s.replace("go to the origin", "go to origin")
    s = s.replace("newgame", "new game")

    # spoken numbers
    replacements = {
        " one ": " 1 ",
        " two ": " 2 ",
        " three ": " 3 ",
        " four ": " 4 ",
        " five ": " 5 ",
        " six ": " 6 ",
        " seven ": " 7 ",
        " eight ": " 8 ",
    }

    s = f" {s} "
    for k, v in replacements.items():
        s = s.replace(k, v)

    s = " ".join(s.split())
    return s

def transcript_to_command(text: str):
    s = normalize_text(text)

    # direct commands
    if s in ("new game", "new"):
        return "new"

    if s in ("origin", "go to origin"):
        return "origin"

    if s in ("zero", "set zero", "go zero"):
        return "zero"

    if s in ("feedback", "give feedback"):
        return "feedback"

    if s in ("mode vs", "versus mode", "play against computer"):
        return "mode vs"

    if s in ("mode manual", "manual mode"):
        return "mode manual"

    if s in ("level easy", "easy level"):
        return "level easy"

    if s in ("level medium", "medium level"):
        return "level medium"

    if s in ("level hard", "hard level"):
        return "level hard"

    # remove filler words for move parsing
    move_text = s
    for w in ["move", "from", "to"]:
        move_text = re.sub(rf"\b{w}\b", " ", move_text)
    move_text = " ".join(move_text.split())

    # pattern: e2 e4
    matches = SQUARE_RE.findall(move_text)
    if len(matches) >= 2:
        from_sq = f"{matches[0][0]}{matches[0][1]}".lower()
        to_sq = f"{matches[1][0]}{matches[1][1]}".lower()
        return f"move {from_sq}{to_sq}"

    # pattern: e2e4
    compact = re.sub(r"[^a-z0-9]", "", move_text)
    m = re.search(r"([a-h][1-8])([a-h][1-8])", compact, re.IGNORECASE)
    if m:
        return f"move {m.group(1).lower()}{m.group(2).lower()}"

    return None

print("Loading model...")
model = WhisperModel("small", device="cpu", compute_type="int8")
print("Ready.")
print("Speak once every 5 seconds. Say 'quit program' to stop.")
print()

while True:
    print("Recording for 5 seconds...")
    audio = sd.rec(
        int(SECONDS * SAMPLERATE),
        samplerate=SAMPLERATE,
        channels=1,
        dtype="int16",
        device=DEVICE
    )
    sd.wait()
    sf.write(FILENAME, audio, SAMPLERATE)

    segments, info = model.transcribe(
        FILENAME,
        language="en",
        beam_size=5,
        vad_filter=True,
        initial_prompt="Chess voice commands like move e2 to e4, new game, go to origin, level easy."
    )

    full_text = ""
    for segment in segments:
        full_text += segment.text + " "

    raw_text = full_text.strip()
    low = normalize_text(raw_text)

    if low == "quit program":
        print("RAW:", raw_text if raw_text else "(empty)")
        print("Stopping.")
        break

    cmd = transcript_to_command(raw_text)

    print("RAW:", raw_text if raw_text else "(empty)")
    print("NORM:", low if low else "(empty)")
    print("CMD:", cmd if cmd else "NOT RECOGNIZED")
    print()