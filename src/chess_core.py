#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import sys
import chess
import chess.engine
from arduino_bridge import ArduinoBridge

STOCKFISH_PATH = r"C:\Users\e-mashine\Desktop\nuc_voice\stockfish\stockfish-windows-x86-64-avx2\stockfish\stockfish-windows-x86-64-avx2.exe"

LEVELS = {
    "easy":   {"think_time": 0.10, "skill": 2},
    "medium": {"think_time": 0.40, "skill": 10},
    "hard":   {"think_time": 1.20, "skill": 18},
}


def safe_input(prompt="> "):
    try:
        sys.stdout.write(prompt)
        sys.stdout.flush()
        line = sys.stdin.buffer.readline()
        if not line:
            return None
        return line.decode("utf-8", errors="ignore").strip()
    except Exception:
        return ""


def set_engine_level(engine: chess.engine.SimpleEngine, level_name: str):
    lv = LEVELS.get(level_name, LEVELS["easy"])
    engine.configure({"Skill Level": int(lv["skill"])})


def parse_move(board: chess.Board, text: str):
    """
    Accepts:
      - UCI: e2e4, g1f3
      - Two squares: e2 e4, e2-e4, e2->e4
      - SAN: Nf3, exd5, O-O
    Returns chess.Move or None.
    """
    s = (text or "").strip()
    if not s:
        return None

    s2 = s.replace("->", " ").replace("-", " ").replace(",", " ").replace("to", " ")
    parts = s2.split()

    if len(parts) == 1:
        token = parts[0]
        try:
            mv = chess.Move.from_uci(token.lower())
            if mv in board.legal_moves:
                return mv
        except Exception:
            pass
        try:
            return board.parse_san(token)
        except Exception:
            return None

    if len(parts) >= 2:
        uci = (parts[0] + parts[1]).lower()
        try:
            mv = chess.Move.from_uci(uci)
            if mv in board.legal_moves:
                return mv
        except Exception:
            return None

    return None


def cp_from_info(info: dict, pov_color: chess.Color):
    if not isinstance(info, dict):
        return None
    sc = info.get("score")
    if sc is None:
        return None
    try:
        s = sc.pov(pov_color)
        if s.is_mate():
            m = s.mate()
            if m is None:
                return None
            return 100000 if m > 0 else -100000
        return s.score(mate_score=100000)
    except Exception:
        return None


def judgement_from_delta(delta_cp: int):
    drop = -delta_cp
    if drop < 20:
        return "good"
    if drop < 80:
        return "ok"
    if drop < 200:
        return "inaccuracy"
    if drop < 500:
        return "mistake"
    return "blunder"


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


def do_physical_move(board: chess.Board, bridge: ArduinoBridge, mv: chess.Move, verbose=True):
    from_sq = chess.square_name(mv.from_square).upper()
    to_sq = chess.square_name(mv.to_square).upper()

    is_capture = board.is_capture(mv)
    is_en_passant = board.is_en_passant(mv)
    is_castling = board.is_castling(mv)

    if is_en_passant:
        print("[WARN] En passant physical handling not implemented yet.")
        return False

    if is_castling:
        print("[WARN] Castling physical handling not implemented yet.")
        return False

    if is_capture:
        captured_piece = board.piece_at(mv.to_square)
        if captured_piece is None:
            print("[ERR] Capture detected but no piece found on target square.")
            return False

        piece_token = piece_to_token(captured_piece)
        if piece_token is None:
            print("[ERR] Unknown captured piece.")
            return False

        bridge.capture_piece(from_sq, to_sq, piece_token, dry_run=False, verbose=verbose)
    else:
        bridge.move_piece(from_sq, to_sq, dry_run=False, verbose=verbose)

    return True


def main():
    board = chess.Board()
    bridge = ArduinoBridge()
    physical_mode = True
    mode = "manual"  # manual or vs
    level = "easy"

    last_player_move = None
    last_player_fen_before = None

    print("=== NUC Chess Core (offline) ===")
    print("Commands: new, zero, origin, mode manual|vs, level easy|medium|hard, move <...>, feedback, fen, board, quit")
    print("Move formats: e2e4 | e2 e4 | SAN like Nf3, exd5, O-O")
    print()

    try:
        engine = chess.engine.SimpleEngine.popen_uci(STOCKFISH_PATH)
    except Exception as e:
        print(f"[ERR] Cannot start stockfish: {e}")
        return

    try:
        bridge.open()
        bridge.ping(verbose=True)
        print("[OK] Arduino bridge connected.")
    except Exception as e:
        print(f"[ERR] Cannot connect to Arduino: {e}")
        try:
            engine.quit()
        except Exception:
            pass
        return

    set_engine_level(engine, level)

    while True:
        cmd = safe_input("> ")
        if cmd is None:
            break
        if not cmd:
            continue

        low = cmd.strip().lower()

        if low in ("q", "quit", "exit"):
            break

        if low == "new":
            board.reset()
            last_player_move = None
            last_player_fen_before = None
            print("[OK] New game.")
            continue

        if low.startswith("mode "):
            parts = low.split(None, 1)
            m = parts[1].strip() if len(parts) > 1 else ""
            if m in ("manual", "vs"):
                mode = m
                print(f"[OK] Mode = {mode}")
            else:
                print("[ERR] mode must be manual or vs")
            continue

        if low.startswith("level "):
            parts = low.split(None, 1)
            lv = parts[1].strip() if len(parts) > 1 else ""
            if lv in LEVELS:
                level = lv
                set_engine_level(engine, level)
                print(f"[OK] Level = {level}")
            else:
                print("[ERR] level must be easy|medium|hard")
            continue

        if low == "fen":
            print(board.fen())
            continue

        if low == "board":
            print(board)
            continue

        if low == "zero":
            try:
                bridge.set_zero(verbose=True)
                print("[OK] Zeroed.")
            except Exception as e:
                print(f"[ERR] Zero failed: {e}")
            continue

        if low == "origin":
            try:
                bridge.go_xy(0, 0, verbose=True)
                print("[OK] Moved to origin (0,0).")
            except Exception as e:
                print(f"[ERR] Origin move failed: {e}")
            continue

        if low.startswith("move "):
            move_text = cmd[5:].strip()
            mv = parse_move(board, move_text)
            if mv is None:
                print("[ERR] Illegal/unknown move.")
                continue

            fen_before = board.fen()

            try:
                if physical_mode:
                    ok = do_physical_move(board, bridge, mv, verbose=True)
                    if not ok:
                        print("[ERR] Physical move not executed.")
                        continue

                board.push(mv)
                last_player_move = mv
                last_player_fen_before = fen_before

                print(f"[OK] Player move: {mv.uci()}")

                if board.is_game_over():
                    print(f"[OK] Game over: {board.result()}")
                    continue

                if mode == "vs":
                    lv = LEVELS.get(level, LEVELS["easy"])
                    result = engine.play(board, chess.engine.Limit(time=lv["think_time"]))
                    cpu_move = result.move

                print("[DEBUG] CPU move uci:", cpu_move.uci())
                print("[DEBUG] CPU move san:", board.san(cpu_move))
                print("[DEBUG] CPU is_capture:", board.is_capture(cpu_move))
                print("[DEBUG] CPU is_en_passant:", board.is_en_passant(cpu_move))
                print("[DEBUG] CPU is_castling:", board.is_castling(cpu_move))

                to_sq_name = chess.square_name(cpu_move.to_square).upper()
                piece_on_to = board.piece_at(cpu_move.to_square)
                print("[DEBUG] CPU target square:", to_sq_name)
                print("[DEBUG] Piece on target BEFORE push:", piece_on_to)
                print("[DEBUG] FEN before CPU move:", board.fen())

                    if cpu_move is None:
                        print("[ERR] Engine returned no move.")
                        continue

                    try:
                        if physical_mode:
                            ok = do_physical_move(board, bridge, cpu_move, verbose=True)
                            if not ok:
                                print("[ERR] CPU physical move not executed.")
                                continue

                        board.push(cpu_move)
                        print(f"[OK] CPU move: {cpu_move.uci()}")

                        if board.is_game_over():
                            print(f"[OK] Game over: {board.result()}")
                    except Exception as e:
                        print(f"[ERR] CPU move failed: {e}")
                        continue

            except Exception as e:
                print(f"[ERR] Move failed: {e}")
            continue

        if low == "feedback":
            if last_player_move is None or last_player_fen_before is None:
                print("[ERR] No player move available for feedback.")
                continue

            try:
                board_before = chess.Board(last_player_fen_before)
                mover_color = board_before.turn

                info_before = engine.analyse(board_before, chess.engine.Limit(time=0.4))
                cp_before = cp_from_info(info_before, mover_color)

                board_after = chess.Board(last_player_fen_before)
                board_after.push(last_player_move)
                info_after = engine.analyse(board_after, chess.engine.Limit(time=0.4))
                cp_after = cp_from_info(info_after, mover_color)

                pv_uci = []
                try:
                    pv = info_before.get("pv", [])
                    pv_uci = [m.uci() for m in pv[:5]]
                except Exception:
                    pv_uci = []

            except Exception as e:
                print(f"[ERR] Feedback failed: {e}")
                continue

            print("=== Feedback (offline) ===")
            print(f"Your move: {last_player_move.uci()}")

            if cp_before is None or cp_after is None:
                print("Eval: (score unavailable on this engine build)")
                if pv_uci:
                    print("Suggested line:", " ".join(pv_uci))
                print("=========================")
                continue

            delta = cp_after - cp_before
            j = judgement_from_delta(delta)

            print(f"Eval before: {cp_before/100:+.2f}, after: {cp_after/100:+.2f} (player POV)")
            print(f"Judgement: {j} (drop ≈ {-delta/100:.2f} pawns for the mover)")
            if pv_uci:
                print("Suggested line:", " ".join(pv_uci))
            print("=========================")
            continue

        print("[ERR] Unknown command.")

    try:
        engine.quit()
    except Exception:
        pass

    try:
        bridge.close()
    except Exception:
        pass

    print("Bye.")


if __name__ == "__main__":
    main()