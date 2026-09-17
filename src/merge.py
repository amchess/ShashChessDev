import os

def clean_position_h():
    filepath = "position.h"
    if not os.path.exists(filepath):
        return
        
    with open(filepath, "r", encoding="utf-8") as f:
        lines = f.readlines()

    # Rimuoviamo TUTTE le occorrenze di see, is_draw_move e has_game_cycle
    cleaned_lines = []
    for line in lines:
        if "Value see(Move" in line or "bool is_draw_move(Move" in line or "bool has_game_cycle(int" in line:
            continue
        cleaned_lines.append(line)

    # Le reinseriamo UNA SOLA VOLTA subito prima di "bool legal(Move m) const;"
    final_lines = []
    for line in cleaned_lines:
        if "bool legal(Move" in line:
            final_lines.append("    Value see(Move m) const;\n")
            final_lines.append("    bool is_draw_move(Move m) const;\n")
            final_lines.append("    bool has_game_cycle(int ply) const;\n\n")
        final_lines.append(line)

    with open(filepath, "w", encoding="utf-8") as f:
        f.writelines(final_lines)
    print("[OK] position.h ripulito dai duplicati.")

def clean_search_h():
    filepath = "search.h"
    if not os.path.exists(filepath):
        return

    with open(filepath, "r", encoding="utf-8") as f:
        lines = f.readlines()

    # Rimuoviamo tutte le variabili che hanno causato problemi (per reinserirle 1 sola volta)
    cleaned_lines = []
    for line in lines:
        if "Depth completedDepth" in line or "bool nmpGuard =" in line or "nmpSide =" in line:
            continue
        if "ShashinManager& getShashinManager()" in line or "int lastShashinUpdatedDepth =" in line:
            continue
        cleaned_lines.append(line)

    # Inseriamole subito prima di "private:" dentro la classe Worker
    final_lines = []
    for line in cleaned_lines:
        # Troviamo il private: della classe Worker (che è l'ultima o penultima classe nel file)
        if "private:" in line and "class Worker" not in line: 
            # Inseriamo i campi
            final_lines.append("    Depth completedDepth;\n")
            final_lines.append("    bool nmpGuard = false, nmpSide = false;\n")
            final_lines.append("    ShashinManager& getShashinManager();\n")
            final_lines.append("    int lastShashinUpdatedDepth = 0;\n\n")
        final_lines.append(line)

    with open(filepath, "w", encoding="utf-8") as f:
        f.writelines(final_lines)
    print("[OK] search.h ripristinato.")

def fix_shashin_position():
    filepath = "shashin/shashin_position.h"
    if not os.path.exists(filepath):
        return
        
    with open(filepath, "r", encoding="utf-8") as f:
        content = f.read()

    # Il casino degli attacks_bb e distance: lo facciamo col replace secco
    content = content.replace("pawn_Attacks::attacks_bb<WHITE>", "Attacks::attacks_bb<PAWN>")
    content = content.replace("pawn_Attacks::attacks_bb<BLACK>", "Attacks::attacks_bb<PAWN>")
    
    # Se il vecchio script lo ha corrotto ulteriormente:
    content = content.replace("Attacks::Attacks::attacks_bb", "Attacks::attacks_bb")
    
    content = content.replace("shift(file_bb(kf, EAST))", "shift(file_bb(kf), EAST)")
    content = content.replace("shift(file_bb(kf, WEST))", "shift(file_bb(kf), WEST)")
    
    # Distance
    content = content.replace("ShashChess::distance(sq, ksq)", "std::max(std::abs(file_of(sq) - file_of(ksq)), std::abs(rank_of(sq) - rank_of(ksq)))")
    content = content.replace("ShashChess::distance(kingSq, sq)", "std::max(std::abs(file_of(kingSq) - file_of(sq)), std::abs(rank_of(kingSq) - rank_of(sq)))")
    content = content.replace("ShashChess::distance(ksq, centerSq)", "std::max(std::abs(file_of(ksq) - file_of(centerSq)), std::abs(rank_of(ksq) - rank_of(centerSq)))")
    
    with open(filepath, "w", encoding="utf-8") as f:
        f.write(content)
    print("[OK] shashin_position.h riparato.")

def main():
    print("Inizio scansione linea per linea...")
    clean_position_h()
    clean_search_h()
    fix_shashin_position()
    print("Fatto. Lancia il make!")

if __name__ == "__main__":
    main()