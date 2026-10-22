// Text the recomp adds in French, German, Italian, Spanish and Indonesian: Double Agent recomp's wording where the
// English is the same, in sentence case with German's nouns capitalized; the panel's rows and the game's menu show it
// in capitals. Indonesian has no game strings on any disc, so only this text changes with it.
#include <algorithm>
#include <cctype>
#include <iterator>
#include <map>
#include <mutex>

#include "common/options.h"
#include "common/translations.h"

namespace {

// in game::kLanguages order after int; an empty one (the touch rows the other languages lack) stays English
struct Entry {
    std::string_view english;
    std::u16string_view text[5];
};
constexpr Entry kEntries[] = {
    // options menu pages and the RECOMP SETTINGS sections
    {"RECOMP SETTINGS", {u"Paramètres Recomp", u"Recomp-Einstellungen", u"Impostazioni Recomp", u"Configuración de Recomp", u"Pengaturan Recomp"}},
    {"MOUSE AND KEYBOARD", {u"Souris et clavier", u"Maus und Tastatur", u"Mouse e tastiera", u"Ratón y teclado", u"Mouse dan keyboard"}},
    {"GENERAL", {u"Général", u"Allgemein", u"Generale", u"General", u"Umum"}},
    {"DISPLAY", {u"Affichage", u"Anzeige", u"Video", u"Pantalla", u"Tampilan"}},
    {"GRAPHICS", {u"Graphismes", u"Grafik", u"Grafica", u"Gráficos", u"Grafis"}},
    {"PAGE", {u"Page", u"Seite", u"Pagina", u"Página", u"Halaman"}},
    {"RESTORE DEFAULTS", {u"Rétablir les valeurs par défaut", u"Standard wiederherstellen", u"Ripristina predefiniti", u"Restaurar predeterminados", u"Kembalikan ke bawaan"}},
    {"PRESS A KEY", {u"Appuyez sur une touche", u"Taste drücken", u"Premi un tasto", u"Pulsa una tecla", u"Tekan tombol"}},
    {"UNBOUND", {u"Non assigné", u"Nicht belegt", u"Non assegnato", u"Sin asignar", u"Belum diatur"}},
    {"TOUCH", {{}, {}, {}, {}, u"Sentuh"}},
    {"TOUCH SENSITIVITY", {u"Sensibilité tactile", u"Touch-Empfindlichkeit", u"Sensibilità touch", u"Sensibilidad táctil", u"Sensitivitas sentuh"}},
    {"DRAG TO MOVE, PINCH OR - + TO RESIZE", {{}, {}, {}, {}, u"Seret untuk memindah, cubit atau - + untuk mengubah ukuran"}},
    {"QUALITY", {u"Qualité", u"Qualität", u"Qualità", u"Calidad", u"Kualitas"}},
    {"PERFORMANCE", {u"Performances", u"Leistung", u"Prestazioni", u"Rendimiento", u"Performa"}},
    {"TOUCH CONTROLS", {u"Commandes tactiles", u"Touch-Steuerung", u"Comandi touch", u"Controles táctiles", u"Kontrol sentuh"}},
    {"MOTION CONTROLS", {u"Commandes de mouvement", u"Bewegungssteuerung", u"Comandi di movimento", u"Controles de movimiento", u"Kontrol gerak"}},
    {"UPDATES", {u"Mises à jour", u"Updates", u"Aggiornamenti", u"Actualizaciones", u"Pembaruan"}},
    {"WEAPONS", {u"Armes", u"Waffen", u"Armi", u"Armas", u"Senjata"}},
    {"PLAYER", {u"Joueur", u"Spieler", u"Giocatore", u"Jugador", u"Pemain"}},
    {"GAME", {u"Jeu", u"Spiel", u"Gioco", u"Juego", u"Permainan"}},

    // RECOMP SETTINGS rows
    {"MONITOR", {u"Écran", u"Monitor", u"Monitor", u"Monitor", u"Monitor"}},
    {"DISPLAY MODE", {u"Mode d'affichage", u"Anzeigemodus", u"Modalità schermo", u"Modo de pantalla", u"Mode tampilan"}},
    {"RESOLUTION", {u"Résolution", u"Auflösung", u"Risoluzione", u"Resolución", u"Resolusi"}},
    {"RENDER RESOLUTION", {u"Résolution de rendu", u"Render-Auflösung", u"Risoluzione di rendering", u"Resolución de renderizado", u"Resolusi render"}},
    {"FIELD OF VIEW", {u"Champ de vision", u"Sichtfeld", u"Campo visivo", u"Campo de visión", u"Bidang pandang"}},
    {"VIEWMODEL FIELD OF VIEW", {u"Champ de vision de l'arme", u"Waffen-Sichtfeld", u"Campo visivo dell'arma", u"Campo de visión del arma", u"Bidang pandang senjata"}},
    {"VERTICAL SYNC", {u"Synchro verticale", u"V-Sync", u"Sincronia verticale", u"Sincronismo vertical", u"Sinkronisasi vertikal"}},
    {"FMV SCALE", {u"Échelle des cinématiques", u"Videoskalierung", u"Scala dei filmati", u"Escala de vídeos", u"Skala video"}},
    {"FILL", {u"Remplir", u"Füllen", u"Riempi", u"Rellenar", u"Penuh"}},
    {"HDR OUTPUT", {u"Sortie HDR", u"HDR-Ausgabe", u"Uscita HDR", u"Salida HDR", u"Keluaran HDR"}},
    {"MAX FRAME RATE", {u"Images par seconde max.", u"Max. Bildrate", u"Frame rate massimo", u"Fotogramas por segundo máx.", u"Frame rate maks."}},
    {"INFINITE ENERGY", {u"Énergie infinie", u"Unendliche Energie", u"Energia infinita", u"Energía infinita", u"Energi tak terbatas"}},
    {"FOG", {u"Brouillard", u"Nebel", u"Nebbia", u"Niebla", u"Kabut"}},
    {"BLOOM", {u"Flou lumineux", u"Bloom", u"Bloom", u"Resplandor", u"Bloom"}},
    {"ULTRAWIDE", {u"Format ultra-large", u"Ultrawide", u"Ultrawide", u"Ultrapanorámico", u"Ultrawide"}},
    {"FRAME SKIP", {u"Saut d'images", u"Bilder überspringen", u"Salto dei frame", u"Salto de fotogramas", u"Lewati frame"}},
    {"FPS COUNTER POSITION", {u"Position du compteur", u"Position der FPS-Anzeige", u"Posizione contatore FPS", u"Posición del contador", u"Posisi penghitung FPS"}},
    {"SHOW CPU / GPU USAGE", {u"Afficher l'utilisation CPU / GPU", u"CPU- / GPU-Auslastung anzeigen", u"Mostra utilizzo CPU / GPU", u"Mostrar uso de CPU / GPU", u"Tampilkan penggunaan CPU / GPU"}},
    {"SHOW TEMPERATURES", {u"Afficher les températures", u"Temperaturen anzeigen", u"Mostra temperature", u"Mostrar temperaturas", u"Tampilkan suhu"}},
    {"TOP CENTER", {u"En haut au centre", u"Oben mittig", u"In alto al centro", u"Arriba al centro", u"Atas tengah"}},
    {"TOP LEFT", {u"En haut à gauche", u"Oben links", u"In alto a sinistra", u"Arriba a la izquierda", u"Kiri atas"}},
    {"TOP RIGHT", {u"En haut à droite", u"Oben rechts", u"In alto a destra", u"Arriba a la derecha", u"Kanan atas"}},
    {"BOTTOM LEFT", {u"En bas à gauche", u"Unten links", u"In basso a sinistra", u"Abajo a la izquierda", u"Kiri bawah"}},
    {"BOTTOM CENTER", {u"En bas au centre", u"Unten mittig", u"In basso al centro", u"Abajo al centro", u"Bawah tengah"}},
    {"BOTTOM RIGHT", {u"En bas à droite", u"Unten rechts", u"In basso a destra", u"Abajo a la derecha", u"Kanan bawah"}},
    {"FPS COUNTER", {u"Compteur d'images par seconde", u"FPS-Anzeige", u"Contatore FPS", u"Contador de FPS", u"Penghitung FPS"}},
    {"ANISOTROPIC FILTERING", {u"Filtrage anisotropique", u"Anisotropische Filterung", u"Filtro anisotropico", u"Filtro anisotrópico", u"Filter anisotropik"}},
    {"DRAW DISTANCE", {u"Distance d'affichage", u"Sichtweite", u"Distanza visiva", u"Distancia de dibujado", u"Jarak pandang"}},
    {"AMBIENT OCCLUSION", {u"Occlusion ambiante", u"Ambient Occlusion", u"Occlusione ambientale", u"Oclusión ambiental", u"Oklusi ambien"}},
    {"RELOAD BLUR", {u"Flou au rechargement", u"Nachlade-Unschärfe", u"Sfocatura in ricarica", u"Desenfoque al recargar", u"Blur saat isi ulang"}},
    {"SKIP INTROS", {u"Passer les intros", u"Intros überspringen", u"Salta introduzione", u"Saltar introducción", u"Lewati intro"}},
    {"PAUSE ON FOCUS LOSS", {u"Pause en arrière-plan", u"Pause im Hintergrund", u"Pausa in background", u"Pausa en segundo plano", u"Jeda di latar belakang"}},
    {"LANGUAGE", {u"Langue", u"Sprache", u"Lingua", u"Idioma", u"Bahasa"}},
    {"AUDIO OUTPUT", {u"Sortie audio", u"Audioausgabe", u"Uscita audio", u"Salida de audio", u"Keluaran audio"}},
    {"CONTROLS LAYOUT", {{}, {}, {}, {}, u"Tata letak kontrol"}},
    {"CONTROLS OPACITY", {{}, {}, {}, {}, u"Transparansi kontrol"}},
    {"STICK MODE", {{}, {}, {}, {}, u"Mode analog"}},
    {"FLOATING", {{}, {}, {}, {}, u"Mengambang"}},
    {"FIXED", {{}, {}, {}, {}, u"Tetap"}},
    {"CONTROLS SIZE", {{}, {}, {}, {}, u"Ukuran kontrol"}},
    {"EDIT CONTROLS POSITION", {{}, {}, {}, {}, u"Ubah posisi kontrol"}},
    {"GYROSCOPE", {{}, {}, {}, {}, u"Giroskop"}},
    {"GYRO SENSITIVITY", {{}, {}, {}, {}, u"Sensitivitas giroskop"}},
    {"CHEATS", {u"Triches", u"Cheats", u"Trucchi", u"Trucos", u"Cheat"}},
    {"CREDITS", {u"Crédits", u"Mitwirkende", u"Crediti", u"Créditos", u"Kredit"}},
    {"ANDROID PORT", {u"Portage Android", u"Android-Portierung", u"Porting Android", u"Port de Android", u"Port Android"}},
    {"RECOMPILER AND RUNTIME", {u"Recompilateur et runtime", u"Recompiler und Laufzeit", u"Ricompilatore e runtime", u"Recompilador y runtime", u"Recompiler dan runtime"}},
    {"LIBRARIES", {u"Bibliothèques", u"Bibliotheken", u"Librerie", u"Bibliotecas", u"Pustaka"}},
    {"XBOX REFERENCES", {u"Références Xbox", u"Xbox-Referenzen", u"Riferimenti Xbox", u"Referencias de Xbox", u"Referensi Xbox"}},
    {"SUPPORT", {u"Soutenir", u"Unterstützen", u"Supporta", u"Apoyar", u"Dukungan"}},
    {"DONATE", {u"Faire un don", u"Spenden", u"Dona", u"Donar", u"Donasi"}},
    {"INFINITE AMMO", {u"Munitions infinies", u"Unendliche Munition", u"Munizioni infinite", u"Munición infinita", u"Amunisi tak terbatas"}},
    {"INFINITE GRENADES", {u"Grenades infinies", u"Unendliche Granaten", u"Granate infinite", u"Granadas infinitas", u"Granat tak terbatas"}},
    {"RAPID FIRE", {u"Tir rapide", u"Schnellfeuer", u"Fuoco rapido", u"Disparo rápido", u"Tembak cepat"}},
    {"NO RECOIL", {u"Sans recul", u"Kein Rückstoß", u"Nessun rinculo", u"Sin retroceso", u"Tanpa recoil"}},
    {"GOD MODE", {u"Invincibilité", u"Unverwundbarkeit", u"Invincibilità", u"Invencibilidad", u"Mode dewa"}},
    {"ONE-HIT KILL", {u"Mort en un coup", u"Ein-Treffer-Kill", u"Uccisione con un colpo", u"Muerte de un golpe", u"Sekali tembak mati"}},
    {"GAME SPEED", {u"Vitesse du jeu", u"Spielgeschwindigkeit", u"Velocità di gioco", u"Velocidad del juego", u"Kecepatan game"}},

    // their values
    {"FULLSCREEN", {u"Plein écran", u"Vollbild", u"Schermo intero", u"Pantalla completa", u"Layar penuh"}},
    {"WINDOWED", {u"Fenêtré", u"Fenster", u"Finestra", u"Ventana", u"Jendela"}},
    {"BORDERLESS", {u"Fenêtré sans bordure", u"Randloses Fenster", u"Finestra senza bordi", u"Ventana sin bordes", u"Jendela tanpa bingkai"}},
    {"ON", {u"Oui", u"Ein", u"Sì", u"Sí", u"Aktif"}},
    {"OFF", {u"Non", u"Aus", u"No", u"No", u"Mati"}},
    {"UNLIMITED", {u"Illimité", u"Unbegrenzt", u"Illimitato", u"Ilimitado", u"Tanpa batas"}},
    {"REFRESH RATE", {u"Fréquence de rafraîchissement", u"Bildwiederholfrequenz", u"Frequenza di aggiornamento", u"Frecuencia de actualización", u"Refresh rate layar"}},
    {"AUTOMATIC", {u"Automatique", u"Automatisch", u"Automatico", u"Automático", u"Otomatis"}},
    {"GAME DEFAULT", {u"Par défaut du jeu", u"Spielstandard", u"Predefinito del gioco", u"Predeterminado del juego", u"Bawaan game"}},
    {"NOT SUPPORTED", {u"Non pris en charge", u"Nicht unterstützt", u"Non supportato", u"No compatible", u"Tidak didukung"}},
    {"MONO", {u"Mono", u"Mono", u"Mono", u"Mono", u"Mono"}},
    {"2.0 STEREO", {u"2.0 Stéréo", u"2.0 Stereo", u"2.0 Stereo", u"2.0 Estéreo", u"2.0 Stereo"}},
    {"5.1 SURROUND", {u"5.1 Surround", u"5.1 Surround", u"5.1 Surround", u"5.1 Envolvente", u"5.1 Surround"}},
    {"(DEFAULT)", {u"(par défaut)", u"(Standard)", u"(predefinito)", u"(por defecto)", u"(bawaan)"}},
    {"(RESTART)", {u"(redémarrage)", u"(Neustart)", u"(riavvio)", u"(reinicio)", u"(mulai ulang)"}},
    {"PRIMARY", {u"Principal", u"Primär", u"Principale", u"Principal", u"Utama"}},
    {"(PRIMARY)", {u"(principal)", u"(primär)", u"(principale)", u"(principal)", u"(utama)"}},
    {"ENGLISH", {u"Anglais", u"Englisch", u"Inglese", u"Inglés", u"Inggris"}},
    {"FRENCH", {u"Français", u"Französisch", u"Francese", u"Francés", u"Prancis"}},
    {"GERMAN", {u"Allemand", u"Deutsch", u"Tedesco", u"Alemán", u"Jerman"}},
    {"ITALIAN", {u"Italien", u"Italienisch", u"Italiano", u"Italiano", u"Italia"}},
    {"SPANISH", {u"Espagnol", u"Spanisch", u"Spagnolo", u"Español", u"Spanyol"}},
    // its own name in every language, so it can be found from any of them
    {"INDONESIAN", {u"Bahasa Indonesia", u"Bahasa Indonesia", u"Bahasa Indonesia", u"Bahasa Indonesia", u"Bahasa Indonesia"}},
    {"ALWAYS", {{}, {}, {}, {}, u"Selalu"}},
    {"WHILE FIRING", {{}, {}, {}, {}, u"Saat menembak"}},
    {"OPEN", {{}, {}, {}, {}, u"Buka"}},
    {"CHECK FOR UPDATES", {u"Rechercher des mises à jour", u"Nach Updates suchen", u"Cerca aggiornamenti", u"Buscar actualizaciones", u"Cek pembaruan"}},
    {"CHECK", {u"Rechercher", u"Suchen", u"Cerca", u"Buscar", u"Cek"}},
    {"CUSTOM", {{}, {}, {}, {}, u"Kustom"}},
    {"DEFAULT", {{}, {}, {}, {}, u"Bawaan"}},
    {"3 FINGER", {{}, {}, {}, {}, u"3 jari"}},
    {"4 FINGER", {{}, {}, {}, {}, u"4 jari"}},

    // MOUSE AND KEYBOARD rows
    {"MOUSE SENSITIVITY", {u"Sensibilité de la souris", u"Mausempfindlichkeit", u"Sensibilità del mouse", u"Sensibilidad del ratón", u"Sensitivitas mouse"}},
    {"INVERT LOOK", {u"Inverser la visée", u"Blick umkehren", u"Inverti visuale", u"Invertir vista", u"Balik arah pandang"}},
    {"HOLD TO AIM", {u"Maintenir pour viser", u"Zum Zielen halten", u"Tieni premuto per mirare", u"Mantener para apuntar", u"Tahan untuk membidik"}},
    {"WALK SPEED", {u"Vitesse de marche", u"Gehgeschwindigkeit", u"Velocità di camminata", u"Velocidad al caminar", u"Kecepatan berjalan"}},
    {"MOVE FORWARD", {u"Avancer", u"Vorwärts bewegen", u"Avanti", u"Avanzar", u"Maju"}},
    {"MOVE BACK", {u"Reculer", u"Rückwärts bewegen", u"Indietro", u"Retroceder", u"Mundur"}},
    {"STRAFE LEFT", {u"Pas latéral à gauche", u"Seitwärts nach links", u"Spostamento laterale a sinistra", u"Desplazamiento lateral a la izquierda", u"Geser ke kiri"}},
    {"STRAFE RIGHT", {u"Pas latéral à droite", u"Seitwärts nach rechts", u"Spostamento laterale a destra", u"Desplazamiento lateral a la derecha", u"Geser ke kanan"}},
    {"WALK", {u"Marcher", u"Gehen", u"Cammina", u"Caminar", u"Berjalan"}},
    {"CROUCH", {u"S'accroupir", u"Ducken", u"Chinati", u"Agacharse", u"Jongkok"}},
    {"FIRE", {u"Tirer", u"Feuern", u"Fuoco", u"Disparar", u"Tembak"}},
    {"AIM", {u"Viser", u"Zielen", u"Mira", u"Apuntar", u"Bidik"}},
    {"MELEE", {u"Corps à corps", u"Nahkampf", u"Corpo a corpo", u"Cuerpo a cuerpo", u"Serangan jarak dekat"}},
    {"RELOAD", {u"Recharger", u"Nachladen", u"Ricarica", u"Recargar", u"Isi ulang"}},
    {"INTERACT", {u"Action", u"Aktion", u"Azione", u"Acción", u"Interaksi"}},
    {"THROW GRENADE", {u"Lancer une grenade", u"Granate werfen", u"Lancia granata", u"Lanzar granada", u"Lempar granat"}},
    {"CHANGE WEAPON UP", {u"Arme suivante", u"Nächste Waffe", u"Arma successiva", u"Arma siguiente", u"Senjata berikutnya"}},
    {"CHANGE WEAPON DOWN", {u"Arme précédente", u"Vorige Waffe", u"Arma precedente", u"Arma anterior", u"Senjata sebelumnya"}},
    {"TOGGLE SUPPRESSOR", {u"Mettre / retirer le silencieux", u"Schalldämpfer an / ab", u"Monta / smonta silenziatore", u"Poner / quitar silenciador", u"Pasang / lepas peredam"}},
    {"FIRING MODE / SCOPE ZOOM", {u"Mode de tir / zoom de la lunette", u"Feuermodus / Zielfernrohr-Zoom", u"Modalità di fuoco / Zoom mirino", u"Modo de disparo / Zoom de la mira", u"Mode tembak / zoom teropong"}},
    {"HEALTH PACK", {u"Trousse de soins", u"Medipack", u"Kit medico", u"Botiquín", u"Kotak medis"}},
    {"NEXT MUSIC TRACK", {u"Morceau suivant", u"Nächster Musiktitel", u"Brano successivo", u"Siguiente pista de música", u"Lagu berikutnya"}},
    {"OBJECTIVES", {u"Objectifs", u"Ziele", u"Obiettivi", u"Objetivos", u"Misi"}},
    {"PAUSE MENU", {u"Menu pause", u"Pausenmenü", u"Menu di pausa", u"Menú de pausa", u"Menu jeda"}},
    {"TOGGLE HUD", {u"Afficher / masquer l'interface", u"HUD ein / aus", u"Mostra / nascondi HUD", u"Mostrar / ocultar HUD", u"Tampilkan / sembunyikan HUD"}},
    {"SETTINGS PANEL", {u"Panneau des paramètres", u"Einstellungsmenü", u"Pannello impostazioni", u"Panel de configuración", u"Panel pengaturan"}},
    {"LOOK UP", {u"Regarder en haut", u"Nach oben sehen", u"Guarda in alto", u"Mirar arriba", u"Lihat ke atas"}},
    {"LOOK DOWN", {u"Regarder en bas", u"Nach unten sehen", u"Guarda in basso", u"Mirar abajo", u"Lihat ke bawah"}},
    {"LOOK LEFT", {u"Regarder à gauche", u"Nach links sehen", u"Guarda a sinistra", u"Mirar a la izquierda", u"Lihat ke kiri"}},
    {"LOOK RIGHT", {u"Regarder à droite", u"Nach rechts sehen", u"Guarda a destra", u"Mirar a la derecha", u"Lihat ke kanan"}},

    // a hint's "PRESS (A) :" with the key in place of the button
    {"PRESS", {u"Appuyez sur", u"Drücke", u"Premi", u"Pulsa", u"Tekan"}},

    // the F10 panel's own
    {"SETTINGS", {u"Paramètres", u"Einstellungen", u"Impostazioni", u"Configuración", u"Pengaturan"}},
    {"BACK", {u"Retour", u"Zurück", u"Indietro", u"Atrás", u"Kembali"}},
    {"DEFAULTS", {u"Par défaut", u"Standard", u"Predefiniti", u"Por defecto", u"Bawaan"}},
    {"APPLY", {u"Appliquer", u"Übernehmen", u"Applica", u"Aplicar", u"Terapkan"}},
    {"MOUSE", {u"Souris", u"Maus", u"Mouse", u"Ratón", u"Mouse"}},
    {"KEYBOARD", {u"Clavier", u"Tastatur", u"Tastiera", u"Teclado", u"Keyboard"}},
    {"Press a key or mouse button to bind, Escape cancels", {u"Appuyez sur une touche ou un bouton de souris, Échap pour annuler", u"Taste oder Maustaste drücken, Escape bricht ab", u"Premi un tasto o un pulsante del mouse, Esc per annullare", u"Pulsa una tecla o un botón del ratón, Escape para cancelar", u"Tekan tombol keyboard atau mouse untuk mengatur, Escape untuk batal"}},
    {"Right click or Delete clears the key", {u"Clic droit ou Suppr pour effacer la touche", u"Rechtsklick oder Entf löscht die Taste", u"Clic destro o Canc per cancellare il tasto", u"Clic derecho o Supr para borrar la tecla", u"Klik kanan atau Delete untuk menghapus tombol"}},
    {"CONTROLLER", {u"Manette", u"Controller", u"Controller", u"Mando", u"Controller"}},
    {"PRESS A BUTTON", {u"Appuyez sur un bouton", u"Taste drücken", u"Premi un pulsante", u"Pulsa un botón", u"Tekan tombol"}},
    {"Press a button on the controller to bind, wait 5 seconds to cancel", {u"Appuyez sur un bouton de la manette, attendez 5 secondes pour annuler", u"Controller-Taste drücken, 5 Sekunden warten bricht ab", u"Premi un pulsante del controller, attendi 5 secondi per annullare", u"Pulsa un botón del mando, espera 5 segundos para cancelar", u"Tekan tombol di controller untuk mengatur, tunggu 5 detik untuk batal"}},
    {"Right click or Delete restores the default", {u"Clic droit ou Suppr pour rétablir la valeur par défaut", u"Rechtsklick oder Entf stellt den Standard wieder her", u"Clic destro o Canc per ripristinare il predefinito", u"Clic derecho o Supr para restaurar el predeterminado", u"Klik kanan atau Delete untuk kembali ke bawaan"}},

    // the touch layout editor
    {"RESET", {{}, {}, {}, {}, u"Atur ulang"}},
    {"CANCEL", {{}, {}, {}, {}, u"Batal"}},
    {"HIDE", {{}, {}, {}, {}, u"Sembunyikan"}},
    {"SHOW", {{}, {}, {}, {}, u"Tampilkan"}},
    {"DONE", {{}, {}, {}, {}, u"Selesai"}},
    {"DRAG A CONTROL TO MOVE IT", {{}, {}, {}, {}, u"Seret kontrol untuk memindahkannya"}},
};

bool Same(std::string_view a, std::string_view b) {
    auto upper = [](char c) { return char(std::toupper(uint8_t(c))); };
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [&](char x, char y) {
               return upper(x) == upper(y);
           });
}

bool WordChar(char c) { return std::isalnum(uint8_t(c)) != 0; }

// the language the game booted in, -1 for English or one this disc cannot boot in
int Language() {
    static const int language = [] {
        if (!game::LanguageAvailable(game::BootLanguage())) return -1;
        auto at = std::find(std::begin(game::kLanguages), std::end(game::kLanguages), game::BootLanguage());
        return at == std::end(game::kLanguages) ? -1 : int(at - std::begin(game::kLanguages)) - 1;
    }();
    return language;
}

// null where the entry has none in this language, so the English shows
const char16_t* Translation(std::string_view english) {
    for (const Entry& e : kEntries)
        if (Same(e.english, english)) return e.text[Language()].empty() ? nullptr : e.text[Language()].data();
    return nullptr;
}

// the longest phrase with a translation from text[at], at a word start; end set past it
const char16_t* PhraseAt(std::string_view text, size_t at, size_t& end) {
    if (at && WordChar(text[at - 1])) return nullptr;
    for (end = text.size(); end > at; end--) {
        if (end < text.size() && WordChar(text[end])) continue;
        if (const char16_t* found = Translation(text.substr(at, end - at))) return found;
    }
    return nullptr;
}

// latin 1 lower case to upper; 0xF7 is the division sign
char16_t Upper(char16_t c) {
    bool lower = (c >= u'a' && c <= u'z') || (c >= 0xE0 && c <= 0xFE && c != 0xF7);
    return lower ? char16_t(c - 0x20) : c;
}

void AppendUtf8(std::string& out, char16_t c) {
    if (c < 0x80) {
        out += char(c);
    } else if (c < 0x800) {
        out += char(0xC0 | c >> 6);
        out += char(0x80 | (c & 0x3F));
    } else {
        out += char(0xE0 | c >> 12);
        out += char(0x80 | ((c >> 6) & 0x3F));
        out += char(0x80 | (c & 0x3F));
    }
}

}  // namespace

std::string app::Ui(std::string_view text, bool caps) {
    if (Language() < 0) return std::string(text);
    // the panel's thread and the game's
    static std::mutex lock;
    static std::map<std::string, std::string> cache;
    std::lock_guard guard(lock);
    std::string key = std::string(text) + (caps ? "\1" : "\2");
    if (auto it = cache.find(key); it != cache.end()) return it->second;
    std::string out;
    size_t i = 0;
    while (i < text.size()) {
        size_t end = 0;
        const char16_t* found = PhraseAt(text, i, end);
        if (!found) {
            out += text[i++];
            continue;
        }
        for (; *found; found++) AppendUtf8(out, caps ? Upper(*found) : *found);
        i = end;
    }
    return cache[key] = out;
}
