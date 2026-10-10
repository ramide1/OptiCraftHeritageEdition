#include "StringTranslate.h"

#include "GameResources.h"
#include "platform/Log.h"
#include <memory>

namespace
{
std::unique_ptr<std::istream> openLanguageResource(const std::string &path)
{
    return GameResources::open(path);
}

bool hasCodepointAtLeast256(const std::string &text)
{
    for (std::size_t i = 0; i < text.size();)
    {
        unsigned char c = (unsigned char)text[i];
        unsigned int codepoint = 0;
        std::size_t length = 1;
        if (c < 0x80)
        {
            codepoint = c;
        }
        else if ((c & 0xe0) == 0xc0 && i + 1 < text.size())
        {
            codepoint = ((c & 0x1f) << 6) | ((unsigned char)text[i + 1] & 0x3f);
            length = 2;
        }
        else if ((c & 0xf0) == 0xe0 && i + 2 < text.size())
        {
            codepoint = ((c & 0x0f) << 12) | (((unsigned char)text[i + 1] & 0x3f) << 6)
                      | ((unsigned char)text[i + 2] & 0x3f);
            length = 3;
        }
        else if ((c & 0xf8) == 0xf0 && i + 3 < text.size())
        {
            codepoint = ((c & 0x07) << 18) | (((unsigned char)text[i + 1] & 0x3f) << 12)
                      | (((unsigned char)text[i + 2] & 0x3f) << 6)
                      | ((unsigned char)text[i + 3] & 0x3f);
            length = 4;
        }
        else
        {
            codepoint = 0x100;
        }
        if (codepoint >= 0x100)
            return true;
        i += length;
    }
    return false;
}

#ifdef PS2_PLATFORM
// Whole-file version of hasCodepointAtLeast256: true only if every value in
// the file (key=value lines; keys are always plain-ASCII identifiers, so
// only values are worth checking) stays inside codepoints 0..255. A missing
// or unreadable file returns false -- exclude rather than guess.
bool languageFileIsLatin1Only(const std::string &path)
{
    std::unique_ptr<std::istream> owned = openLanguageResource(path);
    std::istream *input = owned.get();
    if (input == nullptr || !(*input))
        return false;

    std::string line;
    while (std::getline(*input, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty() || line[0] == '#')
            continue;
        if (hasCodepointAtLeast256(line))
            return false;
    }
    return true;
}
#endif
}

StringTranslate *StringTranslate::instance = nullptr;

StringTranslate::StringTranslate()
    : currentLanguage()
    , unicode(false)
{
    loadLanguageList();
    setLanguage("en_US");
}

StringTranslate *StringTranslate::getInstance()
{
    if (instance == nullptr)
        instance = new StringTranslate();
    return instance;
}

const std::map<std::string, std::string> &StringTranslate::getLanguageList() const
{
    return languageList;
}

void StringTranslate::filterToLatinLanguagesOnPs2()
{
#ifdef PS2_PLATFORM
    if (latinFiltered)
        return;
    latinFiltered = true;

    for (auto it = languageList.begin(); it != languageList.end(); )
    {
        // en_US is always ASCII by construction (it's the mandatory fallback
        // loaded first in setLanguage()); skip the redundant file read.
        if (it->first == "en_US" || languageFileIsLatin1Only("/lang/" + it->first + ".lang"))
            ++it;
        else
            it = languageList.erase(it);
    }

    if (languageList.find("en_US") == languageList.end())
        languageList["en_US"] = "English (US)";
#endif
}

bool StringTranslate::isLatin1SafeLanguageOnPs2(const std::string &language)
{
#ifdef PS2_PLATFORM
    if (language == "en_US")
        return true;
    return languageFileIsLatin1Only("/lang/" + language + ".lang");
#else
    (void)language;
    return true;
#endif
}

void StringTranslate::loadLanguageList()
{
    languageList.clear();
    std::unique_ptr<std::istream> owned = openLanguageResource("/lang/languages.txt");
    std::istream *input = owned.get();
    if (input != nullptr)
    {
        std::string line;
        while (std::getline(*input, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            std::size_t equals = line.find('=');
            if (equals == std::string::npos)
                continue;
            std::string key = trim(line.substr(0, equals));
            std::string value = trim(line.substr(equals + 1));
            if (!key.empty() && !value.empty())
                languageList[key] = value;
        }
    }
    if (languageList.empty())
        languageList["en_US"] = "English (US)";
}

void StringTranslate::setLanguage(const std::string &language)
{
    if (language == currentLanguage && !translateTable.empty())
        return;

    translateTable.clear();
    englishUiKeys.clear();
    loadLanguageFile("/lang/en_US.lang");
    for (const auto &entry : translateTable)
    {
        const auto dot = entry.first.find('.');
        const std::string group = entry.first.substr(0, dot);
        if (group == "gui" || group == "menu" || group == "options" || group == "controls" ||
            group == "selectWorld" || group == "createWorld" || group == "gameMode" ||
            group == "multiplayer" || group == "disconnect" || group == "connect" ||
            group == "container" || group == "key" || group == "deathScreen")
            englishUiKeys.emplace(entry.second, entry.first);
    }
    const char *aliases[][2] = {
        {"Play Game", "menu.singleplayer"}, {"Help & Options", "menu.options"},
        {"Start Game", "selectWorld.title"}, {"Create New World", "selectWorld.create"},
        {"Resume Game", "menu.returnToGame"}, {"Save & Quit", "menu.returnToMenu"},
        {"Video", "options.video"}, {"Controls", "options.controls"},
        {"Language", "options.language"}, {"Fancy Graphics", "options.graphics"},
        {"Smooth Lighting", "options.ao"}, {"View Bobbing", "options.viewBobbing"},
        {"Render Clouds", "options.renderClouds"}, {"Render Distance", "options.renderDistance"},
        {"FOV", "options.fov"}, {"Sensitivity", "options.sensitivity"},
        {"Invert Mouse", "options.invertMouse"}, {"Attack", "key.attack"},
        {"Use", "key.use"}, {"Jump", "key.jump"}, {"Sneak", "key.sneak"},
        {"Drop", "key.drop"}, {"Inventory", "key.inventory"},
        {"ON", "options.on"}, {"OFF", "options.off"}
    };
    for (const auto &alias : aliases)
        if (translateTable.find(alias[1]) != translateTable.end())
            englishUiKeys[alias[0]] = alias[1];
    englishUiKeys["Crafting"] = "key.crafting";
    currentLanguage = "en_US";
    if (language != "en_US")
    {
        // Try to load the actual .lang file first. If it fails but this is a
        // Spanish variant (es_*), still accept it so the hardcoded Spanish
        // fallbacks in translateUi/translateKey can be used.
        bool isSpanishVariant = (language.size() >= 3 &&
            (language[0] == 'e' || language[0] == 'E') &&
            (language[1] == 's' || language[1] == 'S') &&
            (language[2] == '_' || language[2] == '-'));
        if (loadLanguageFile("/lang/" + language + ".lang") || isSpanishVariant)
            currentLanguage = language;
    }
    // Supplemental files use English labels as keys and do not alter the
    // existing resource files. Missing files/entries keep the English text.
    loadLanguageFile("/lang/ui/" + currentLanguage + ".lang", true);
    updateUnicodeFlag();
}

const std::string &StringTranslate::getCurrentLanguage() const
{
    return currentLanguage;
}

bool StringTranslate::isUnicode() const
{
    return unicode;
}

bool StringTranslate::isBidirectional(const std::string &language)
{
    return language == "ar_SA" || language == "he_IL";
}

std::string StringTranslate::translateUi(const std::string &english)
{
    auto extra = translateTable.find("ui." + english);
    if (extra != translateTable.end() && !extra->second.empty())
        return extra->second;
    // Source labels may have an ellipsis or a colon that the vanilla key lacks.
    const std::string suffix = english.size() >= 3 && english.compare(english.size() - 3, 3, "...") == 0
        ? "..." : (english.size() >= 2 && english.compare(english.size() - 2, 2, ": ") == 0 ? ": " : "");
    if (!suffix.empty())
        return translateUi(english.substr(0, english.size() - suffix.size())) + suffix;
    bool isSpanish = false;
    if (currentLanguage.size() >= 3)
    {
        char c0 = currentLanguage[0];
        char c1 = currentLanguage[1];
        char c2 = currentLanguage[2];
        if ((c0 == 'e' || c0 == 'E') && (c1 == 's' || c1 == 'S') && (c2 == '_' || c2 == '-'))
            isSpanish = true;
    }
    if (isSpanish)
    {
        if (english == "Split Screen") return "Pantalla dividida";
        if (english == "Horizontal") return "Horizontal";
        if (english == "Vertical") return "Vertical";
        if (english == "Toggle") return "Alternar";
        if (english == "Delete") return "Eliminar";
        if (english == "QR Download") return "Descarga QR";
        if (english == "Download") return "Descargar";
        if (english == "Cancel") return "Cancelar";
        if (english == "Back") return "Volver";
        if (english == "OK") return "OK";
        if (english == "Play World") return "Jugar";
        if (english == "Rename World") return "Renombrar";
        if (english == "Delete World") return "Eliminar";
        if (english == "Version") return "Versión";
        if (english == "Auto") return "Automática";
        if (english == "Microsoft Account") return "Cuenta Microsoft";
        if (english == "Account") return "Cuenta";
        if (english == "Login") return "Iniciar sesión";
        if (english == "Logout") return "Cerrar sesión";
        if (english == "Retry") return "Reintentar";
        if (english == "Logged in as") return "Sesión de";
        if (english == "Login complete") return "Sesión iniciada";
        if (english == "Contacting Microsoft") return "Contactando a Microsoft";
        if (english == "Open this address on any device") return "Abrí esta dirección en otro dispositivo";
        if (english == "and enter this code") return "e ingresá este código";
        if (english == "Signing in to Xbox and Minecraft") return "Iniciando sesión en Xbox y Minecraft";
        if (english == "Create New World") return "Crear Mundo";
        if (english == "Select World") return "Seleccionar Mundo";
        if (english == "Open World") return "Abrir Mundo";
        if (english == "Game Mode") return "Modo Juego";
        if (english == "Survival") return "Supervivencia";
        if (english == "Hardcore") return "Extremo";
        if (english == "Peaceful") return "Pacífico";
        if (english == "Difficulty") return "Dificultad";
        if (english == "More World Options") return "Más Opciones de Mundo";
        if (english == "World Size") return "Tamaño de Mundo";
        if (english == "World Type") return "Tipo de Mundo";
        if (english == "Generate Structures") return "Generar Estructuras";
        if (english == "Map Features") return "Características del Mapa";
        if (english == "Map Type") return "Tipo de Mapa";
        if (english == "View") return "Cámara";
        if (english == "Press START Button") return "Presiona START";
        if (english == "Initializing world") return "Iniciando mundo";
        if (english == "Play Tutorial") return "Jugar Tutorial";
        if (english == "ON") return "ACTIVADO";
        if (english == "OFF") return "DESACTIVADO";
        if (english == "Done") return "Hecho";
        if (english == "Cancel") return "Cancelar";
        if (english == "A") return "A";
        if (english == "B") return "B";
        if (english == "X") return "X";
        if (english == "Y") return "Y";
        if (english == "Select") return "Seleccionar";
        if (english == "Start") return "START";
        if (english == "Left Shoulder") return "L";
        if (english == "Right Shoulder") return "R";
        // Creative tab names (vanilla 1.2.5)
        if (english == "creativeTab.blocks") return "Bloques de construcción";
        if (english == "creativeTab.decorations") return "Decoraciones";
        if (english == "creativeTab.redstone") return "Redstone y transporte";
        if (english == "creativeTab.misc") return "Materiales y miscelánea";
        if (english == "creativeTab.food") return "Comida y alquimia";
        if (english == "creativeTab.combat") return "Herramientas, Armas y Armaduras";
        // Debug options
        if (english == "Debug Options") return "Opciones de desarrollo";
        if (english == "Show FPS") return "Mostrar FPS";
        if (english == "F3 Extended Info") return "Información extendida F3";
        if (english == "Set Day") return "Establecer Día";
        if (english == "Keep Inventory") return "Mantener Inventario";
        if (english == "Kill Entities") return "Eliminar Entidades";
        // OptiCraft Options
        if (english == "OptiCraft Options") return "Opciones de OptiCraft";
        if (english == "OptiCraft Options...") return "Opciones de OptiCraft...";
        // Texture packs
        if (english == "Texture Packs") return "Packs de Texturas";
        if (english == "Texture Packs...") return "Packs de Texturas...";
        // Microsoft login
        if (english == "Enter Code") return "Ingresar Código";
        if (english == "Code") return "Código";
        if (english == "Copy") return "Copiar";
        if (english == "Opening browser") return "Abriendo navegador";
        if (english == "Please wait") return "Por favor espere";
        if (english == "Authentication failed") return "Error de autenticación";
        if (english == "Could not connect") return "No se pudo conectar";
        if (english == "Invalid code") return "Código inválido";
        if (english == "Code expired") return "Código expirado";
        if (english == "User cancelled") return "Usuario canceló";
        // World selection
        if (english == "No Games Found") return "No se encontraron mundos";
        // Legacy crafting
        if (english == "Structures") return "Estructuras";
        if (english == "Inventory") return "Inventario";
        if (english == "Missing Items") return "Objetos faltantes";
        if (english == "Crafting") return "Fabricación";
        if (english == "Search") return "Buscar";
        // Legacy UI options
        if (english == "Debug Cheats") return "Trucos de depuración";
        if (english == "Legacy UI") return "IU LCE";
        if (english == "Touch Map") return "Mapa táctil";
        if (english == "Legacy Look") return "Aspecto LCE";
        if (english == "Touch Coords") return "Coordenadas táctiles";
        if (english == "Legacy Crafting") return "Fabricación LCE";
        if (english == "Swap Touch HUD") return "Intercambiar HUD táctil";
        if (english == "Legacy Creative") return "Creativo LCE";
        if (english == "Auto Jump") return "Auto salto";
        if (english == "Toggle Shift") return "Alternar agacharse";
        if (english == "Touch Click") return "Clic táctil";
        if (english == "Face Camera") return "Control de Cámara";
        if (english == "Alternative Controls") return "Controles alternativos";
        if (english == "Deadzone Settings") return "Drift";
        // Legacy crafting actions
        if (english == "Category") return "Categoría";
        if (english == "Navigate") return "Navegar";
        if (english == "Craft/Move") return "Fabricar/Mover";
        // Video settings
        if (english == "Fancy Graphics") return "Gráficos detallados";
        if (english == "Smooth Lighting") return "Iluminación suave";
        if (english == "Invert Mouse") return "Invertir ratón";
        if (english == "Render Distance") return "Distancia de renderizado";
        if (english == "View Bobbing") return "Balanceo de vista";
        if (english == "Render Clouds") return "Renderizar nubes";
        if (english == "FOV") return "Campo de visión";
        if (english == "Sensitivity") return "Sensibilidad";
        // Tutorial world messages
        if (english == "Tutorial world loader is not available") return "El cargador del mundo tutorial no está disponible";
        if (english == "Tutorial level.dat could not be parsed") return "No se pudo analizar level.dat del tutorial";
        if (english == "Tutorial world must be pre-converted to Anvil 19133") return "El mundo tutorial debe preconvertirse a Anvil 19133";
        if (english == "Tutorial World") return "Mundo Tutorial";
        if (english == "Tutorial world could not be opened") return "No se pudo abrir el mundo tutorial";
        if (english == "Welcome to OptiCraft Heritage! Enjoy exploring this world :)!") return "¡Bienvenido a OptiCraft Heritage! ¡Disfruta explorando este mundo :)!";
        // Help options
        if (english == "Controls") return "Controles";
        if (english == "Language") return "Idioma";
        if (english == "View") return "Ver";
        // Microsoft login errors
        if (english == "Sign in with a Microsoft account to use your profile name and join online-mode servers.") return "Inicia sesión con una cuenta Microsoft para usar tu nombre de perfil y unirte a servidores en modo online.";
        if (english == "This Microsoft account has no Xbox profile. Buy and start Minecraft on it once at minecraft.net first.") return "Esta cuenta Microsoft no tiene perfil Xbox. Compra e inicia Minecraft en minecraft.net primero.";
        if (english == "Xbox Live is not available in this account's country.") return "Xbox Live no está disponible en el país de esta cuenta.";
        if (english == "This account must confirm its age at login.live.com first.") return "Esta cuenta debe confirmar su edad en login.live.com primero.";
        if (english == "This account reached its playtime limit and cannot sign in.") return "Esta cuenta alcanzó su límite de tiempo de juego y no puede iniciar sesión.";
        if (english == "This is a child account without a family setup. Configure it at account.microsoft.com first.") return "Esta es una cuenta infantil sin configuración familiar. Configúrala en account.microsoft.com primero.";
        if (english == "This account was banned by Xbox and cannot sign in.") return "Esta cuenta fue baneada por Xbox y no puede iniciar sesión.";
        if (english == "This family-restricted account needs a guardian to allow online play first.") return "Esta cuenta con restricciones familiares necesita un tutor que permita el juego online primero.";
        if (english == "This account must accept the Xbox terms of use first.") return "Esta cuenta debe aceptar los términos de uso de Xbox primero.";
        if (english == "Microsoft's login service could not be reached") return "No se pudo contactar al servicio de inicio de sesión de Microsoft";
        if (english == "Microsoft's login service sent an unreadable answer") return "El servicio de inicio de sesión de Microsoft envió una respuesta ilegible";
        if (english == "Microsoft refused to start the sign-in") return "Microsoft rechazó iniciar la sesión";
        if (english == "Microsoft's login service sent an incomplete answer") return "El servicio de inicio de sesión de Microsoft envió una respuesta incompleta";
        if (english == "The sign-in code expired before the login finished") return "El código de inicio de sesión expiró antes de terminar";
        if (english == "This build was configured without a Microsoft application id (OPTICRAFT_MSA_CLIENT_ID)") return "Esta compilación no tiene ID de aplicación Microsoft (OPTICRAFT_MSA_CLIENT_ID)";
        // QR Download messages
        if (english == "Point the back camera at a QR code") return "Apunta la cámara trasera a un código QR";
        if (english == "Waiting for camera frames...") return "Esperando frames de la cámara...";
        if (english == "(A) Add    (B) Cancel") return "(A) Añadir    (B) Cancelar";
        if (english == "(A) Download    (B) Cancel") return "(A) Descargar    (B) Cancelar";
        if (english == "Installing...") return "Instalando...";
        if (english == "(a few seconds)") return "(unos segundos)";
        if (english == "Texture pack installed") return "Pack de texturas instalado";
        if (english == "Select it in Options > Texture Packs") return "Selecciónalo en Opciones > Packs de Texturas";
        if (english == "Mod installed") return "Mod instalado";
        if (english == "The mod could not be installed") return "No se pudo instalar el mod";
        if (english == "The downloaded file is not a skin, texture pack or mod") return "El archivo descargado no es un skin, pack de texturas o mod";
        if (english == "Skin") return "Skin";
        if (english == "Texture pack") return "Pack de texturas";
        if (english == "Mod") return "Mod";
        if (english == "Minecraft server") return "Servidor Minecraft";
        if (english == "Unknown file") return "Archivo desconocido";
        // Skin selector
        if (english == "Default Skins") return "Skins Originales";
        if (english == "Custom") return "Personalizado";
        if (english == "Choose 2nd Player Skin") return "Elegir Skin 2do Jugador";
        if (english == "Load Skins") return "Cargar Skins";
        if (english == "Delete Skin") return "Eliminar Skin";
        if (english == "Load Skins from Storage") return "Cargar Skins desde Almacenamiento";
        if (english == "Place standard .png skins in a 'skins' folder") return "Coloca skins .png estándar en una carpeta 'skins'";
        if (english == "Load from Device (Recommended)") return "Cargar desde Dispositivo (Recomendado)";
        if (english == "Load from USB Storage") return "Cargar desde USB";
        // Mods menu
        if (english == "Mod Manager") return "Gestor de Mods";
        if (english == "Load Mods") return "Cargar Mods";
        if (english == "Delete Mod") return "Eliminar Mod";
        if (english == "Settings") return "Ajustes";
        if (english == "Are you sure you want to delete this mod?") return "¿Seguro que quieres eliminar este mod?";
        if (english == "No mods installed.") return "No hay mods instalados.";
        if (english == "Click 'Load Mods' to install .ochpack mods.") return "Haz clic en 'Cargar Mods' para instalar mods .ochpack.";
        if (english == "[ ENABLED ]") return "[ ACTIVADO ]";
        if (english == "[ DISABLED ]") return "[ DESACTIVADO ]";
        if (english == "Install") return "Instalar";
        if (english == "Update / Reinstall") return "Actualizar / Reinstalar";
        if (english == "By") return "Por";
        if (english == "Unknown") return "Desconocido";
        if (english == "Package Version") return "Versión del paquete";
        if (english == "Installed Version") return "Versión instalada";
        if (english == "None (Not Installed)") return "Ninguna (No instalado)";
        if (english == "Load") return "Cargar";
        if (english == "Tab") return "Pestaña";
        if (english == "Sent as /login and /register when the server asks") return "Se envía como /login y /register cuando el servidor lo pide";
        // Texture packs default
        if (english == "Default") return "Predeterminado";
        if (english == "The default look of Minecraft") return "El aspecto predeterminado de Minecraft";
        // Language navigation
        if (english == "Next") return "Siguiente";
        if (english == "Previous") return "Anterior";
        // Player name
        if (english == "Player Name") return "Nombre de Jugador";
        // Additional UI strings
        if (english == "Install Mod Package") return "Instalar Paquete de Mods";
        if (english == "Auto-Login Settings") return "Ajustes de Auto-Login";
        if (english == "Server password:") return "Contraseña del servidor:";
        if (english == "Wii Pad Bindings") return "Controles Wii Pad";
        if (english == "HERITAGE EDITION") return "EDICIÓN HERENCIA";
        if (english == "Install Custom Skin") return "Instalar Skin Personalizado";
        if (english == "Will be saved to: ") return "Se guardará en: ";
        if (english == "Size: ") return "Tamaño: ";
        if (english == "Select a skin to preview") return "Selecciona un skin para ver";
        if (english == "Play Game") return "Jugar";
        if (english == "Help & Options") return "Ayuda y Opciones";
        if (english == "Creative") return "Creativo";
        if (english == "Easy") return "Fácil";
        if (english == "Normal") return "Normal";
        if (english == "Hard") return "Difícil";
        if (english == "Game Mode: ") return "Modo de Juego: ";
        if (english == "Render Distance: ") return "Distancia de Renderizado: ";
        if (english == "FOV: ") return "Campo de Visión: ";
        if (english == "Sensitivity: ") return "Sensibilidad: ";
        if (english == "Reset to Defaults") return "Restablecer Valores";
        if (english == "Controls") return "Controles";
        if (english == "Select a command to edit.") return "Selecciona un comando para editar.";
        if (english == "Press a button for") return "Presiona un botón para";
        if (english == "Press a button...") return "Presiona un botón...";
        if (english == "Press a key...") return "Presiona una tecla...";
        if (english == "Jump") return "Saltar";
        if (english == "Sneak") return "Agacharse";
        if (english == "Attack") return "Atacar";
        if (english == "Use") return "Usar";
        if (english == "Drop") return "Soltar";
        if (english == "Inventory") return "Inventario";
        if (english == "Classic") return "Clásico";
        if (english == "Could not load Tutorial World") return "No se pudo cargar el mundo tutorial";
        if (english == "Play World") return "Jugar";
        if (english == "Rename World") return "Renombrar";
        if (english == "Delete World") return "Eliminar";
        // In-game menu
        if (english == "Resume Game") return "Continuar";
        if (english == "Achievements") return "Logros";
        if (english == "Statistics") return "Estadísticas";
        if (english == "Disconnect") return "Desconectar";
        if (english == "Save & Quit") return "Guardar y Salir";
        if (english == "Saving level..") return "Guardando nivel..";
        if (english == "Game menu") return "Menú del juego";
        // Mods
        if (english == "Load Mods (.ochpack)") return "Cargar Mods (.ochpack)";
        if (english == "Select the storage location to scan for mods") return "Selecciona el almacenamiento para buscar mods";
        if (english == "Select an .ochpack mod to view details and install") return "Selecciona un mod .ochpack para ver detalles e instalar";
        if (english == "Install Selected") return "Instalar Seleccionado";
        if (english == "Search:") return "Buscar:";
        if (english == "Loading...") return "Cargando...";
        if (english == "No mods found. Place .ochpack files in the mods folder.") return "No hay mods. Coloca archivos .ochpack en la carpeta mods.";
        if (english == "Compatible:") return "Compatible:";
        if (english == "Requires:") return "Requiere:";
        // Legacy crafting
        if (english == "Recipe") return "Receta";
        if (english == "Ready [Cross]") return "Listo [Cruz]";
        if (english == "Ready [A]") return "Listo [A]";
        if (english == "Ready [Enter]") return "Listo [Enter]";
        if (english == "Needs Crafting Table") return "Necesita Mesa de Crafteo";
        // Skin selector extra
        if (english == "64x64 (Modern)") return "64x64 (Moderno)";
        if (english == "64x32 (Classic)") return "64x32 (Clásico)";
        if (english == "Install Skin") return "Instalar Skin";
        // Mods list extra
        if (english == "No .ochpack mod files found") return "No se encontraron archivos .ochpack";
        if (english == "Place .ochpack files in") return "Coloca archivos .ochpack en";
        if (english == "The device 'mods' folder") return "La carpeta 'mods' del dispositivo";
        if (english == "USB:/mods") return "USB:/mods";
        if (english == "Available Mods (Device)") return "Mods disponibles (dispositivo)";
        if (english == "No .ochpack packages found on device.") return "No se encontraron paquetes .ochpack en el dispositivo.";
        if (english == "Place .ochpack files in the 'mods' folder next to the ELF.") return "Coloca archivos .ochpack en la carpeta 'mods' junto al ELF.";
        if (english == "Available Mods (USB Storage)") return "Mods disponibles (USB)";
        if (english == "No .ochpack packages found on USB storage.") return "No se encontraron paquetes .ochpack en el USB.";
        if (english == "Checked: mass:/ and mass:/mods/. Ensure USB is connected.") return "Verificado: mass:/ y mass:/mods/. Asegúrate de que el USB esté conectado.";
        if (english == "[ UPDATE ]") return "[ ACTUALIZAR ]";
        if (english == "[ INSTALLED ]") return "[ INSTALADO ]";
        if (english == "[ AVAILABLE ]") return "[ DISPONIBLE ]";
        // Other missing
        if (english == "Skins") return "Aspectos";
        if (english == "Mods") return "Mods";
        // QR download result messages
        if (english == "Server added to the multiplayer list") return "Servidor añadido a la lista";
        if (english == "The server could not be added") return "No se pudo añadir el servidor";
        if (english == "Skin installed and selected") return "Skin instalado y seleccionado";
        if (english == "The skin could not be installed") return "No se pudo instalar el skin";
        if (english == "Could not convert the texture pack for this console") return "No se pudo convertir el pack de texturas para esta consola";
        if (english == "The texture pack could not be saved") return "No se pudo guardar el pack de texturas";
        // Save-conflict / out-of-memory screens
        if (english == "Back to title screen") return "Volver al menú principal";
        if (english == "Level save conflict") return "Conflicto de guardado del nivel";
        if (english == "Minecraft detected a conflict in the level save data.") return "Minecraft detectó un conflicto en los datos de guardado del nivel.";
        if (english == "This could be caused by two copies of the game") return "Puede deberse a que dos copias del juego están";
        if (english == "accessing the same level.") return "accediendo al mismo nivel.";
        if (english == "To prevent level corruption, the current game has quit.") return "Para evitar la corrupción del nivel, el juego actual se cerró.";
        if (english == "Out of memory!") return "¡Sin memoria!";
        if (english == "Minecraft has run out of memory.") return "Minecraft se ha quedado sin memoria.";
        if (english == "This could be caused by a bug in the game or by the") return "Puede deberse a un error del juego o a que la";
        if (english == "Java Virtual Machine not being allocated enough") return "Máquina Virtual de Java no tiene suficiente";
        if (english == "memory. If you are playing in a web browser, try") return "memoria asignada. Si juegas en un navegador, prueba";
        if (english == "downloading the game and playing it offline.") return "a descargar el juego y jugar sin conexión.";
        if (english == "We've tried to free up enough memory to let you go back to") return "Se intentó liberar suficiente memoria para volver";
        if (english == "the main menu and back to playing, but this may not have worked.") return "al menú principal y seguir jugando, pero puede que no haya funcionado.";
        if (english == "Please restart the game if you see this message again.") return "Reinicia el juego si ves este mensaje de nuevo.";
        if (english == "Deleting world") return "Eliminando mundo";
        if (english == "Edit sign message:") return "Editar mensaje del cartel:";
        // Loading screens
        if (english == "Respawning") return "Reapareciendo";
        if (english == "Building terrain") return "Construyendo terreno";
        if (english == "Preparing terrain") return "Preparando terreno";
        if (english == "Simulating world for a bit") return "Simulando el mundo un momento";
        if (english == "This may take a while :)") return "Esto puede tardar un poco :)";
        if (english == "Saving chunks") return "Guardando chunks";
        if (english == "Generating level") return "Generando nivel";
        if (english == "Loading level") return "Cargando nivel";
        if (english == "Entering the Nether") return "Entrando al Nether";
        if (english == "Leaving the Nether") return "Saliendo del Nether";
        if (english == "Entering the End") return "Entrando al Fin";
        if (english == "Leaving the End") return "Saliendo del Fin";
        // General-stat display names (stat names bake to the boot locale at
        // StatList init; these keep them translatable at draw time).
        if (english == "Times Played") return "Veces jugado";
        if (english == "Worlds created") return "Mundos creados";
        if (english == "Worlds loaded") return "Mundos cargados";
        if (english == "Multiplayer joins") return "Conexiones multijugador";
        if (english == "Games quit") return "Partidas cerradas";
        if (english == "Minutes Played") return "Minutos jugados";
        if (english == "Distance Walked") return "Distancia andada";
        if (english == "Distance Fallen") return "Distancia caída";
        if (english == "Distance Swum") return "Distancia nadada";
        if (english == "Distance Flown") return "Distancia volada";
        if (english == "Distance Dove") return "Distancia buceada";
        if (english == "Distance by Minecart") return "Distancia en vagoneta";
        if (english == "Distance by Boat") return "Distancia en barca";
        if (english == "Distance by Pig") return "Distancia en cerdo";
        if (english == "Jumps") return "Saltos";
        if (english == "Items Dropped") return "Objetos soltados";
        if (english == "Things Dropped") return "Objetos soltados";
        if (english == "Damage Dealt") return "Daño infligido";
        if (english == "Damage Taken") return "Daño recibido";
        if (english == "Number of Deaths") return "Muertes";
        if (english == "Mob Kills") return "Criaturas eliminadas";
        if (english == "Player Kills") return "Jugadores eliminados";
        if (english == "Fish Caught") return "Peces pescados";
    }
    // Vanilla label aliases go last: the hardcoded Spanish fallbacks above must
    // win over the English table value, or an es_* variant without a shipped
    // .lang file resolves every alias (menu.*, gui.*, options.*, ...) back to
    // English and none of the UI translations ever fire for it.
    auto key = englishUiKeys.find(english);
    if (key != englishUiKeys.end())
        return translateKey(key->second);
    return english;
}

std::string StringTranslate::translateKey(const std::string &s)
{
    // Check if current language is Spanish (es_XX or es-XX)
    bool isSpanish = false;
    if (currentLanguage.size() >= 3)
    {
        char c0 = currentLanguage[0];
        char c1 = currentLanguage[1];
        char c2 = currentLanguage[2];
        if ((c0 == 'e' || c0 == 'E') && (c1 == 's' || c1 == 'S') && (c2 == '_' || c2 == '-'))
            isSpanish = true;
    }

    // Key-only Spanish fallbacks: apply whether or not the key exists in the
    // loaded table, so es_* variants without a shipped .lang file (the
    // hardcoded-fallback path, e.g. es_AR) get the same coverage as variants
    // backed by a real file (e.g. es_ES).
    if (isSpanish)
    {
        if (s == "creativeTab.blocks") return "Bloques de construcción";
        if (s == "creativeTab.decorations") return "Decoraciones";
        if (s == "creativeTab.redstone") return "Redstone y transporte";
        if (s == "creativeTab.misc") return "Materiales y miscelánea";
        if (s == "creativeTab.food") return "Comida y alquimia";
        if (s == "creativeTab.combat") return "Herramientas, Armas y Armaduras";
        if (s == "gameMode.survival") return "Supervivencia";
        if (s == "gameMode.creative") return "Creativo";
        if (s == "gameMode.hardcore") return "Extremo";
        if (s == "gameMode.changed") return "Tu modo de juego ha cambiado";
        if (s == "options.difficulty.peaceful") return "Pacífico";
        if (s == "options.difficulty.easy") return "Fácil";
        if (s == "options.difficulty.normal") return "Normal";
        if (s == "options.difficulty.hard") return "Difícil";
        if (s == "options.difficulty.hardcore") return "Extremo";
        if (s == "texturePack.title") return "Seleccionar pack de texturas";
        if (s == "texturePack.openFolder") return "Abrir carpeta de packs";
        if (s == "texturePack.folderInfo") return "Coloca los packs de texturas aquí";
        // Generic menu / death-screen keys consumed through translateKey directly.
        if (s == "menu.singleplayer") return "Un jugador";
        if (s == "menu.multiplayer") return "Multijugador";
        if (s == "menu.options") return "Opciones...";
        if (s == "menu.quit") return "Salir del juego";
        if (s == "gui.toMenu") return "Volver al menú principal";
        if (s == "deathScreen.title") return "¡Has muerto!";
        if (s == "deathScreen.title.hardcore") return "¡Fin del juego!";
        if (s == "deathScreen.hardcoreInfo") return "¡No puedes reaparecer en el modo Extremo!";
        if (s == "deathScreen.score") return "Puntuación";
        if (s == "deathScreen.respawn") return "Reaparecer";
        if (s == "deathScreen.titleScreen") return "Menú principal";
        if (s == "deathScreen.deleteWorld") return "Eliminar mundo";
        // Common GUI buttons and container titles
        if (s == "gui.done") return "Hecho";
        if (s == "gui.cancel") return "Cancelar";
        if (s == "gui.yes") return "Sí";
        if (s == "gui.no") return "No";
        if (s == "gui.back") return "Volver";
        if (s == "container.chest") return "Cofre";
        if (s == "container.chestDouble") return "Cofre grande";
        if (s == "container.crafting") return "Fabricación";
        if (s == "container.furnace") return "Horno";
        if (s == "container.dispenser") return "Dispensador";
        if (s == "container.enchant") return "Encantar";
        if (s == "container.inventory") return "Inventario";
        if (s == "container.brewing") return "Soporte de pociones";
        if (s == "container.minecart") return "Vagoneta con cofre";
        // Multiplayer / server list screens
        if (s == "multiplayer.title") return "Juego multijugador";
        if (s == "selectServer.title") return "Lista de servidores";
        if (s == "selectServer.select") return "Entrar al servidor";
        if (s == "selectServer.direct") return "Conexión directa";
        if (s == "selectServer.add") return "Añadir";
        if (s == "selectServer.edit") return "Editar";
        if (s == "selectServer.delete") return "Eliminar";
        if (s == "selectServer.refresh") return "Actualizar";
        if (s == "selectServer.defaultName") return "Servidor de Minecraft";
        if (s == "selectServer.deleteQuestion") return "¿Estás seguro de que quieres eliminar este servidor?";
        if (s == "selectServer.deleteWarning") return "¡se perderá para siempre! (mucho tiempo)";
        if (s == "selectServer.deleteButton") return "Eliminar";
        if (s == "selectServer.empty") return "vacío";
        if (s == "addServer.title") return "Editar información del servidor";
        if (s == "addServer.enterName") return "Nombre del servidor";
        if (s == "addServer.enterIp") return "Dirección del servidor";
        if (s == "addServer.add") return "Hecho";
        if (s == "addServer.hideAddress") return "Ocultar dirección";
        // World selection (core keys)
        if (s == "selectWorld.title") return "Seleccionar mundo";
        if (s == "selectWorld.world") return "Mundo";
        if (s == "selectWorld.conversion") return "Debe ser convertido";
        if (s == "selectWorld.select") return "Jugar en el mundo seleccionado";
        if (s == "selectWorld.create") return "Crear nuevo mundo";
        if (s == "selectWorld.rename") return "Renombrar";
        if (s == "selectWorld.delete") return "Eliminar";
        if (s == "selectWorld.deleteQuestion") return "¿Seguro que quieres eliminar este mundo?";
        if (s == "selectWorld.deleteWarning") return "¡se perderá para siempre! (mucho tiempo)";
        if (s == "selectWorld.deleteButton") return "Eliminar";
        if (s == "selectWorld.empty") return "vacío";
        // Connection / disconnection
        if (s == "connect.connecting") return "Conectando...";
        if (s == "connect.authorizing") return "Iniciando sesión...";
        if (s == "connect.failed") return "No se pudo conectar al servidor";
        if (s == "disconnect.disconnected") return "Desconectado por el servidor";
        if (s == "disconnect.lost") return "Conexión perdida";
        if (s == "disconnect.kicked") return "Expulsado del servidor";
        if (s == "disconnect.closed") return "Conexión cerrada";
        if (s == "disconnect.endOfStream") return "Fin del stream";
        if (s == "disconnect.overflow") return "Desbordamiento del búfer";
        if (s == "disconnect.timeout") return "Tiempo de lectura agotado";
        if (s == "disconnect.genericReason") return "%s";
        if (s == "disconnect.loginFailedInfo") return "Error al iniciar sesión: %s";
        if (s == "disconnect.loginFailedInfo.invalidSession") return "Sesión inválida (intentá reiniciar el juego)";
        // Misc screens
        if (s == "options.languageWarning") return "Puede que las traducciones no sean 100% precisas";
        if (s == "menu.game") return "Menú del juego";
        // In-game menu (vanilla keys, consumed through translateKey directly)
        if (s == "menu.returnToGame") return "Volver al juego";
        if (s == "menu.returnToMenu") return "Guardar y salir al menú principal";
        if (s == "menu.disconnect") return "Desconectar";
        if (s == "gui.achievements") return "Logros";
        if (s == "multiplayer.stopSleeping") return "Dejar de dormir";
        // Link confirmation dialog
        if (s == "chat.link.confirm") return "¿Quieres abrir este enlace?";
        if (s == "chat.copy") return "Copiar al portapapeles";
        if (s == "chat.link.warning") return "¡Nunca abras enlaces de gente que no conoces!";
        // Key bindings (Controls screen reads the key directly)
        if (s == "key.attack") return "Atacar";
        if (s == "key.use") return "Usar";
        if (s == "key.forward") return "Avanzar";
        if (s == "key.left") return "Izquierda";
        if (s == "key.back") return "Retroceder";
        if (s == "key.right") return "Derecha";
        if (s == "key.jump") return "Saltar";
        if (s == "key.inventory") return "Inventario";
        if (s == "key.drop") return "Soltar";
        if (s == "key.chat") return "Chat";
        if (s == "key.playerlist") return "Lista de jugadores";
        if (s == "key.pickItem") return "Coger bloque";
        if (s == "key.fog") return "Niebla";
        if (s == "key.sneak") return "Agacharse";
        if (s == "key.crafting") return "Fabricación";
        // Sleep / bed chat messages and the achievement toast
        if (s == "tile.bed.occupied") return "Esta cama está ocupada";
        if (s == "tile.bed.noSleep") return "Solo puedes dormir de noche";
        if (s == "tile.bed.notSafe") return "No puedes dormir ahora, hay monstruos cerca";
        if (s == "tile.bed.notValid") return "Tu cama no está presente u obstruida";
        if (s == "achievement.taken") return "¡Logro conseguido!";
        if (s == "achievement.get") return "¡Logro conseguido!";
        // Enchantment names (enchanting table tooltips)
        if (s == "enchantment.protection") return "Protección";
        if (s == "enchantment.fireProtection") return "Protección contra el fuego";
        if (s == "enchantment.fallProtection") return "Caída de pluma";
        if (s == "enchantment.explosionProtection") return "Protección contra explosiones";
        if (s == "enchantment.projectileProtection") return "Protección contra proyectiles";
        if (s == "enchantment.oxygen") return "Respiración acuática";
        if (s == "enchantment.waterWorker") return "Afinidad acuática";
        if (s == "enchantment.sharpness") return "Filo";
        if (s == "enchantment.smite") return "Castigo";
        if (s == "enchantment.baneOfArthropods") return "Perdición de los artrópodos";
        if (s == "enchantment.knockback") return "Empuje";
        if (s == "enchantment.fire") return "Aspecto ígneo";
        if (s == "enchantment.lootBonus") return "Saqueo";
        if (s == "enchantment.digging") return "Eficiencia";
        if (s == "enchantment.untouching") return "Toque de seda";
        if (s == "enchantment.durability") return "Irrompibilidad";
        if (s == "enchantment.lootBonusDigger") return "Fortuna";
        if (s == "enchantment.arrowDamage") return "Poder";
        if (s == "enchantment.arrowKnockback") return "Retroceso";
        if (s == "enchantment.arrowFire") return "Llama";
        if (s == "enchantment.arrowInfinite") return "Infinidad";
        // Potion effect names (tooltip lines)
        if (s == "potion.moveSpeed") return "Velocidad";
        if (s == "potion.moveSlowdown") return "Lentitud";
        if (s == "potion.digSpeed") return "Prisa minera";
        if (s == "potion.digSlowDown") return "Fatiga minera";
        if (s == "potion.damageBoost") return "Fuerza";
        if (s == "potion.heal") return "Curación instantánea";
        if (s == "potion.harm") return "Daño instantáneo";
        if (s == "potion.jump") return "Salto potenciado";
        if (s == "potion.confusion") return "Náuseas";
        if (s == "potion.regeneration") return "Regeneración";
        if (s == "potion.resistance") return "Resistencia";
        if (s == "potion.fireResistance") return "Resistencia al fuego";
        if (s == "potion.waterBreathing") return "Respiración acuática";
        if (s == "potion.invisibility") return "Invisibilidad";
        if (s == "potion.blindness") return "Ceguera";
        if (s == "potion.nightVision") return "Visión nocturna";
        if (s == "potion.hunger") return "Hambre";
        if (s == "potion.weakness") return "Debilidad";
        if (s == "potion.poison") return "Veneno";
        if (s == "potion.empty") return "Vacía";
        if (s == "potion.prefix.grenade") return "Arrojadiza";
        if (s == "potion.prefix.splash") return "Arrojadiza";
        // Flavor prefixes (vanilla 1.2.5 list)
        if (s == "potion.prefix.mundane") return "Ordinaria";
        if (s == "potion.prefix.uninteresting") return "Sin interés";
        if (s == "potion.prefix.bland") return "Sosa";
        if (s == "potion.prefix.clear") return "Clara";
        if (s == "potion.prefix.milky") return "Lechosa";
        if (s == "potion.prefix.diffuse") return "Difusa";
        if (s == "potion.prefix.artless") return "Simple";
        if (s == "potion.prefix.thin") return "Aguada";
        if (s == "potion.prefix.awkward") return "Extraña";
        if (s == "potion.prefix.flat") return "Plana";
        if (s == "potion.prefix.bulky") return "Abultada";
        if (s == "potion.prefix.bungling") return "Chapucera";
        if (s == "potion.prefix.buttered") return "Mantecosa";
        if (s == "potion.prefix.smooth") return "Delicada";
        if (s == "potion.prefix.suave") return "Suave";
        if (s == "potion.prefix.debonair") return "Galante";
        if (s == "potion.prefix.thick") return "Espesa";
        if (s == "potion.prefix.elegant") return "Elegante";
        if (s == "potion.prefix.fancy") return "Fina";
        if (s == "potion.prefix.charming") return "Encantadora";
        if (s == "potion.prefix.dashing") return "Distinguida";
        if (s == "potion.prefix.refined") return "Depurada";
        if (s == "potion.prefix.cordial") return "Cordial";
        if (s == "potion.prefix.sparkling") return "Espumosa";
        if (s == "potion.prefix.potent") return "Potente";
        if (s == "potion.prefix.foul") return "Repugnante";
        if (s == "potion.prefix.odorless") return "Inodora";
        if (s == "potion.prefix.rank") return "Fétida";
        if (s == "potion.prefix.harsh") return "Áspera";
        if (s == "potion.prefix.acrid") return "Acre";
        if (s == "potion.prefix.gross") return "Gruesa";
        if (s == "potion.prefix.stinky") return "Apestosa";
        // Stats screen
        if (s == "gui.stats") return "Estadísticas";
        if (s == "stat.generalButton") return "General";
        if (s == "stat.blocksButton") return "Bloques";
        if (s == "stat.itemsButton") return "Objetos";
        if (s == "stat.crafted") return "Veces fabricado";
        if (s == "stat.used") return "Veces usado";
        if (s == "stat.depleted") return "Veces desgastado";
        // Options screen labels and values (vanilla 1.2.5 keys)
        if (s == "options.title") return "Opciones...";
        if (s == "options.video") return "Opciones de video...";
        if (s == "options.videoTitle") return "Opciones de video";
        if (s == "options.music") return "Música y sonido";
        if (s == "options.sound") return "Sonido";
        if (s == "options.on") return "ACTIVADO";
        if (s == "options.off") return "DESACTIVADO";
        if (s == "options.difficulty") return "Dificultad";
        if (s == "options.difficulty.peaceful") return "Pacífico";
        if (s == "options.difficulty.easy") return "Fácil";
        if (s == "options.difficulty.normal") return "Normal";
        if (s == "options.difficulty.hard") return "Difícil";
        if (s == "options.renderDistance") return "Distancia de renderizado";
        if (s == "options.renderDistance.far") return "Lejano";
        if (s == "options.renderDistance.normal") return "Normal";
        if (s == "options.renderDistance.short") return "Corto";
        if (s == "options.renderDistance.tiny") return "Diminuto";
        if (s == "options.guiScale") return "Interfaz gráfica";
        if (s == "options.guiScale.auto") return "Automática";
        if (s == "options.guiScale.small") return "Pequeña";
        if (s == "options.guiScale.normal") return "Normal";
        if (s == "options.guiScale.large") return "Grande";
        if (s == "options.particles") return "Partículas";
        if (s == "options.particles.all") return "Todas";
        if (s == "options.particles.decreased") return "Reducidas";
        if (s == "options.particles.minimal") return "Mínimas";
        if (s == "options.graphics") return "Gráficos";
        if (s == "options.graphics.fancy") return "Detallados";
        if (s == "options.graphics.fast") return "Rápidos";
        if (s == "options.sensitivity") return "Sensibilidad";
        if (s == "options.sensitivity.min") return "*hiposensibilidad*";
        if (s == "options.sensitivity.max") return "¡¡HIPERSENSIBILIDAD!!";
        if (s == "options.fov") return "Campo de visión";
        if (s == "options.fov.min") return "Normal";
        if (s == "options.fov.max") return "Quake Pro";
        if (s == "options.limitFramerate") return "Límite de FPS";
        if (s == "performance.max") return "FPS máximos";
        if (s == "performance.balanced") return "Equilibrado";
        if (s == "performance.powersaver") return "Ahorro de energía";
        if (s == "options.controls") return "Controles...";
        if (s == "options.language") return "Idioma...";
    }

    auto it = translateTable.find(s);
    if (it != translateTable.end())
    {
        const std::string &value = it->second;
        // If we're in Spanish mode and the value is the English fallback, use our hardcoded Spanish translation instead
        if (isSpanish)
        {
            // Game mode translations - Spanish fallbacks for selectWorld screen
            if (s == "selectWorld.gameMode.survival" && value == "Survival") return "Supervivencia";
            if (s == "selectWorld.gameMode.hardcore" && value == "Hardcore") return "Extremo";
            if (s == "selectWorld.gameMode.creative" && value == "Creative") return "Creativo";
            if (s == "selectWorld.gameMode.survival.line1" && value == "Mine, craft, and survive") return "Minar, fabricar y sobrevivir";
            if (s == "selectWorld.gameMode.survival.line2" && value == "against the monsters of the night") return "contra los monstruos de la noche";
            if (s == "selectWorld.gameMode.hardcore.line1" && value == "One life. Hardcore mode.") return "Una vida. Modo Extremo.";
            if (s == "selectWorld.gameMode.hardcore.line2" && value == "The world is deleted on death.") return "El mundo se borra al morir.";
            if (s == "selectWorld.gameMode.creative.line1" && value == "Unlimited resources. No health or hunger.") return "Recursos ilimitados. Sin salud ni hambre.";
            if (s == "selectWorld.gameMode.creative.line2" && value == "Fly by double-tapping Jump.") return "Vuela haciendo doble toque a Saltar.";
            // World type translations
            if (s == "generator.default" && value == "Default") return "Predeterminado";
            if (s == "generator.flat" && value == "Superflat") return "Superplano";
            if (s == "generator.default_1_1" && value == "Default (1.1)") return "Predeterminado (1.1)";
            // Item/block name translations (Spanish)
            if (s == "item.beefRaw.name") return "Bife crudo";
            if (s == "item.beefCooked.name") return "Bife";
            if (s == "item.porkchopRaw.name") return "Carne de cerdo cruda";
            if (s == "item.porkchopCooked.name") return "Carne de cerdo";
            if (s == "item.chickenRaw.name") return "Pollo crudo";
            if (s == "item.chickenCooked.name") return "Pollo cocinado";
            if (s == "item.fishRaw.name") return "Pescado crudo";
            if (s == "item.fishCooked.name") return "Pescado cocinado";
            if (s == "item.cookie.name") return "Galleta";
            if (s == "item.melon.name") return "Melón";
            if (s == "item.apple.name") return "Manzana";
            if (s == "item.appleGold.name") return "Manzana dorada";
            if (s == "item.bread.name") return "Pan";
            if (s == "item.mushroomStew.name") return "Estofado de champiñones";
            if (s == "item.bowl.name") return "Cuenco";
            if (s == "item.cake.name") return "Tarta";
            if (s == "item.sugar.name") return "Azúcar";
            if (s == "item.rottenFlesh.name") return "Carne podrida";
            if (s == "item.spiderEye.name") return "Ojo de araña";
            // Blocks (reporte)
            if (s == "tile.pumpkin.name") return "Calabaza";
            if (s == "tile.litpumpkin.name") return "Linterna de calabaza";
            if (s == "tile.endPortal.name") return "Portal del End";
            if (s == "tile.endPortalFrame.name") return "Marco de portal del End";
            if (s == "tile.dragonEgg.name") return "Huevo de dragón";
            if (s == "tile.tnt.name") return "Dinamita";
            if (s == "tile.dispenser.name") return "Dispensador";
            if (s == "tile.notGate.name") return "Antorcha de redstone";
            if (s == "tile.stairsWood.name") return "Escaleras de madera";
            if (s == "tile.stairsStone.name") return "Escaleras de piedra";
            if (s == "tile.stairsBrick.name") return "Escaleras de ladrillo";
            if (s == "tile.stairsStoneBrickSmooth.name") return "Escaleras de ladrillo de piedra";
            if (s == "tile.stairsNetherBrick.name") return "Escaleras de ladrillo del Nether";
            if (s == "tile.fence.name") return "Cerca";
            if (s == "tile.fenceGate.name") return "Puerta de cerca";
            if (s == "tile.fenceIron.name") return "Barrotes de hierro";
            if (s == "tile.netherFence.name") return "Cerca del Nether";
            if (s == "tile.button.name") return "Botón";
            if (s == "tile.minecart.name") return "Vagoneta";
            // Seeds and farming
            if (s == "item.seeds_pumpkin.name") return "Semillas de calabaza";
            if (s == "item.seeds_melon.name") return "Semillas de melón";
            if (s == "item.seeds.name") return "Semillas";
            if (s == "item.wheat.name") return "Trigo";
            // Spawn eggs
            if (s == "item.monsterPlacer.name") return "Generador";
            if (s == "item.monsterPlacer.ocelot.name") return "Generador de ocelote";
            if (s == "item.monsterPlacer.mooshroom.name") return "Generador de champivaca";
            if (s == "item.monsterPlacer.pigman.name") return "Generador de hombre cerdo zombi";
            if (s == "item.monsterPlacer.pig.name") return "Generador de cerdo";
            if (s == "item.monsterPlacer.cow.name") return "Generador de vaca";
            if (s == "item.monsterPlacer.sheep.name") return "Generador de oveja";
            if (s == "item.monsterPlacer.chicken.name") return "Generador de pollo";
            if (s == "item.monsterPlacer.wolf.name") return "Generador de lobo";
            if (s == "item.monsterPlacer.villager.name") return "Generador de aldeano";
            if (s == "item.monsterPlacer.squid.name") return "Generador de calamar";
            if (s == "item.monsterPlacer.creeper.name") return "Generador de creeper";
            if (s == "item.monsterPlacer.skeleton.name") return "Generador de esqueleto";
            if (s == "item.monsterPlacer.zombie.name") return "Generador de zombi";
            if (s == "item.monsterPlacer.spider.name") return "Generador de araña";
            if (s == "item.monsterPlacer.slime.name") return "Generador de slime";
            if (s == "item.monsterPlacer.ghast.name") return "Generador de ghast";
            if (s == "item.monsterPlacer.enderman.name") return "Generador de enderman";
            if (s == "item.monsterPlacer.silverfish.name") return "Generador de pez plata";
            if (s == "item.monsterPlacer.caveSpider.name") return "Generador de araña de caverna";
            if (s == "item.monsterPlacer.blaze.name") return "Generador de blaze";
            if (s == "item.monsterPlacer.lavaSlime.name") return "Generador de cubo de magma";
            if (s == "potion.prefix.splash.name") return "arrojadiza";
            // Dyes
            if (s == "item.dyePowder.red.name") return "Tinte rojo";
            if (s == "item.dyePowder.green.name") return "Tinte verde";
            if (s == "item.dyePowder.yellow.name") return "Tinte amarillo";
            if (s == "item.dyePowder.blue.name") return "Tinte azul";
            if (s == "item.dyePowder.purple.name") return "Tinte morado";
            if (s == "item.dyePowder.cyan.name") return "Tinte cian";
            if (s == "item.dyePowder.black.name") return "Tinte negro";
            if (s == "item.dyePowder.brown.name") return "Tinte marrón";
            if (s == "item.dyePowder.silver.name") return "Tinte gris claro";
            if (s == "item.dyePowder.gray.name") return "Tinte gris";
            if (s == "item.dyePowder.pink.name") return "Tinte rosa";
            if (s == "item.dyePowder.lime.name") return "Tinte lima";
            if (s == "item.dyePowder.lightBlue.name") return "Tinte azul claro";
            if (s == "item.dyePowder.magenta.name") return "Tinte magenta";
            if (s == "item.dyePowder.orange.name") return "Tinte naranja";
            if (s == "item.dyePowder.white.name") return "Tinte blanco";
            // Blocks with wood variants
            if (s == "tile.wood.oak.name") return "Madera de roble";
            if (s == "tile.wood.spruce.name") return "Madera de abeto";
            if (s == "tile.wood.birch.name") return "Madera de abedul";
            if (s == "tile.wood.jungle.name") return "Madera de jungla";
            if (s == "tile.leaves.oak.name") return "Hojas de roble";
            if (s == "tile.leaves.spruce.name") return "Hojas de abeto";
            if (s == "tile.leaves.birch.name") return "Hojas de abedul";
            if (s == "tile.leaves.jungle.name") return "Hojas de jungla";
            if (s == "tile.sapling.oak.name") return "Brote de roble";
            if (s == "tile.sapling.spruce.name") return "Brote de abeto";
            if (s == "tile.sapling.birch.name") return "Brote de abedul";
            if (s == "tile.sapling.jungle.name") return "Brote de jungla";
            if (s == "tile.planks.oak.name") return "Tablas de roble";
            if (s == "tile.planks.spruce.name") return "Tablas de abeto";
            if (s == "tile.planks.birch.name") return "Tablas de abedul";
            if (s == "tile.planks.jungle.name") return "Tablas de jungla";
            if (s == "tile.stonebricksmooth.name") return "Ladrillo de piedra";
            if (s == "tile.stonebricksmooth.mossy.name") return "Ladrillo de piedra musgoso";
            if (s == "tile.stonebricksmooth.cracked.name") return "Ladrillo de piedra agrietado";
            if (s == "tile.stonebricksmooth.chiseled.name") return "Ladrillo de piedra tallado";
            if (s == "tile.sandStone.chiseled.name") return "Arenisca tallada";
            if (s == "tile.sandStone.smooth.name") return "Arenisca lisa";
            if (s == "tile.mushroom.brown.name") return "Champiñón marrón";
            if (s == "tile.mushroom.red.name") return "Champiñón rojo";
            // Tools
            if (s == "item.charcoal.name") return "Carbón vegetal";
            if (s == "item.coal.name") return "Carbón";
            if (s == "item.swordWood.name") return "Espada de madera";
            if (s == "item.shovelWood.name") return "Pala de madera";
            if (s == "item.pickaxeWood.name") return "Pico de madera";
            if (s == "item.hatchetWood.name") return "Hacha de madera";
            if (s == "item.hoeWood.name") return "Azada de madera";
            if (s == "item.shovelStone.name") return "Pala de piedra";
            if (s == "item.pickaxeStone.name") return "Pico de piedra";
            if (s == "item.hatchetStone.name") return "Hacha de piedra";
            if (s == "item.hoeStone.name") return "Azada de piedra";
            if (s == "item.swordStone.name") return "Espada de piedra";
            if (s == "item.shovelIron.name") return "Pala de hierro";
            if (s == "item.pickaxeIron.name") return "Pico de hierro";
            if (s == "item.hatchetIron.name") return "Hacha de hierro";
            if (s == "item.hoeIron.name") return "Azada de hierro";
            if (s == "item.swordIron.name") return "Espada de hierro";
            if (s == "item.shovelDiamond.name") return "Pala de diamante";
            if (s == "item.pickaxeDiamond.name") return "Pico de diamante";
            if (s == "item.hatchetDiamond.name") return "Hacha de diamante";
            if (s == "item.hoeDiamond.name") return "Azada de diamante";
            if (s == "item.swordDiamond.name") return "Espada de diamante";
            if (s == "item.shovelGold.name") return "Pala de oro";
            if (s == "item.pickaxeGold.name") return "Pico de oro";
            if (s == "item.hatchetGold.name") return "Hacha de oro";
            if (s == "item.hoeGold.name") return "Azada de oro";
            if (s == "item.swordGold.name") return "Espada de oro";
            // Common items
            if (s == "item.stick.name") return "Palo";
            if (s == "item.arrow.name") return "Flecha";
            if (s == "item.bow.name") return "Arco";
            if (s == "item.flintAndSteel.name") return "Mechero";
            if (s == "item.flint.name") return "Pedernal";
            if (s == "item.feather.name") return "Pluma";
            if (s == "item.string.name") return "Hilo";
            if (s == "item.sulphur.name") return "Pólvora";
            if (s == "item.gunpowder.name") return "Pólvora";
            if (s == "item.leather.name") return "Cuero";
            if (s == "item.bootsCloth.name") return "Botas de cuero";
            if (s == "item.leggingsCloth.name") return "Pantalones de cuero";
            if (s == "item.chestplateCloth.name") return "Peto de cuero";
            if (s == "item.helmetCloth.name") return "Casco de cuero";
            if (s == "item.bucket.name") return "Cubo";
            if (s == "item.bucketWater.name") return "Cubo de agua";
            if (s == "item.bucketLava.name") return "Cubo de lava";
            if (s == "item.milk.name") return "Leche";
            if (s == "item.painting.name") return "Pintura";
            if (s == "item.sign.name") return "Cartel";
            if (s == "item.saddle.name") return "Silla";
            if (s == "item.bone.name") return "Hueso";
            if (s == "item.slimeball.name") return "Bola de slime";
            if (s == "item.egg.name") return "Huevo";
            if (s == "item.redstone.name") return "Redstone";
            if (s == "item.glowstone.name") return "Piedra luminosa";
            if (s == "item.compass.name") return "Brújula";
            if (s == "item.clock.name") return "Reloj";
            if (s == "item.fishingRod.name") return "Caña de pescar";
            if (s == "item.emptyMap.name") return "Mapa vacío";
            if (s == "item.map.name") return "Mapa";
            if (s == "item.book.name") return "Libro";
            if (s == "item.paper.name") return "Papel";
            if (s == "item.reeds.name") return "Caña de azúcar";
            if (s == "item.seeds.name") return "Semillas";
            if (s == "item.seeds_pumpkin.name") return "Semillas de calabaza";
            if (s == "item.seeds_melon.name") return "Semillas de melón";
            if (s == "item.wheat.name") return "Trigo";
            if (s == "item.clay.name") return "Arcilla";
            if (s == "item.brick.name") return "Ladrillo";
            if (s == "item.emerald.name") return "Diamante";
            if (s == "item.ingotIron.name") return "Lingote de hierro";
            if (s == "item.ingotGold.name") return "Lingote de oro";
            if (s == "item.netherStalkSeeds.name") return "Semillas de verrugas del Nether";
            if (s == "item.blazeRod.name") return "Vara de blaze";
            if (s == "item.blazePowder.name") return "Polvo de blaze";
            if (s == "item.ghastTear.name") return "Lágrima de ghast";
            if (s == "item.enderPearl.name") return "Perla de ender";
            if (s == "item.eyeOfEnder.name") return "Ojo de ender";
            if (s == "item.fireball.name") return "Carga de fuego";
            if (s == "item.netherbrick.name") return "Ladrillo del Nether";
            if (s == "item.netherrack.name") return "Netherrack";
            if (s == "item.glassBottle.name") return "Botella de vidrio";
            if (s == "item.potion.name") return "Poción";
            if (s == "item.cauldron.name") return "Caldero";
            if (s == "item.brewingStand.name") return "Soporte de pociones";
            if (s == "tile.enchantmentTable.name") return "Mesa de encantamientos";
            if (s == "tile.brewingStand.name") return "Soporte de pociones";
            if (s == "tile.cauldron.name") return "Caldero";
            if (s == "tile.whiteStone.name") return "Piedra del End";
            if (s == "tile.netherBrick.name") return "Ladrillo del Nether";
            if (s == "tile.mycel.name") return "Micelio";
            if (s == "tile.waterlily.name") return "Lirio acuático";
            if (s == "tile.vine.name") return "Enredadera";
            if (s == "tile.melon.name") return "Melón";
            if (s == "tile.cactus.name") return "Cactus";
            if (s == "tile.pumpkinStem.name") return "Tallo de calabaza";
            if (s == "tile.melonStem.name") return "Tallo de melón";
            // Entities
            if (s == "entity.Creeper.name") return "Creeper";
            if (s == "entity.Skeleton.name") return "Esqueleto";
            if (s == "entity.Spider.name") return "Araña";
            if (s == "entity.Zombie.name") return "Zombi";
            if (s == "entity.Slime.name") return "Slime";
            if (s == "entity.Ghast.name") return "Ghast";
            if (s == "entity.PigZombie.name") return "Hombre cerdo zombi";
            if (s == "entity.Enderman.name") return "Enderman";
            if (s == "entity.CaveSpider.name") return "Araña de caverna";
            if (s == "entity.Silverfish.name") return "Pez plata";
            if (s == "entity.Blaze.name") return "Blaze";
            if (s == "entity.LavaSlime.name") return "Cubo de magma";
            if (s == "entity.EnderDragon.name") return "Dragón del End";
            if (s == "entity.Pig.name") return "Cerdo";
            if (s == "entity.Sheep.name") return "Oveja";
            if (s == "entity.Cow.name") return "Vaca";
            if (s == "entity.Chicken.name") return "Pollo";
            if (s == "entity.Squid.name") return "Calamar";
            if (s == "entity.Wolf.name") return "Lobo";
            if (s == "entity.MushroomCow.name") return "Champivaca";
            if (s == "entity.SnowMan.name") return "Muñeco de nieve";
            if (s == "entity.Ozelot.name") return "Ocelote";
            if (s == "entity.VillagerGolem.name") return "Golem de hierro";
            if (s == "entity.Villager.name") return "Aldeano";
            if (s == "entity.Minecart.name") return "Vagoneta";
            if (s == "entity.Boat.name") return "Barca";
            if (s == "entity.Painting.name") return "Pintura";
            if (s == "entity.Item.name") return "Objeto";
            if (s == "entity.XPOrb.name") return "Orbe de experiencia";
            if (s == "entity.ThrownEnderpearl.name") return "Perla de ender lanzada";
            if (s == "entity.EyeOfEnderSignal.name") return "Ojo de ender";
            if (s == "entity.ThrownPotion.name") return "Poción lanzada";
            if (s == "entity.ThrownExpBottle.name") return "Botella de experiencia lanzada";
            if (s == "entity.Arrow.name") return "Flecha";
            if (s == "entity.Snowball.name") return "Bola de nieve";
            if (s == "entity.Fireball.name") return "Bola de fuego";
            if (s == "entity.SmallFireball.name") return "Bola de fuego pequeña";
            if (s == "entity.Monster.name") return "Monstruo";
            if (s == "entity.Mob.name") return "Mob";
            if (s == "entity.PrimedTnt.name") return "TNT";
            if (s == "entity.FallingSand.name") return "Arena cayendo";
            if (s == "entity.EnderCrystal.name") return "Cristal del End";
            // Achievements
            if (s == "achievement.openInventory") return "Haciendo inventario";
            if (s == "achievement.openInventory.desc") return "Abre tu inventario";
            if (s == "achievement.mineWood") return "Consigue madera";
            if (s == "achievement.mineWood.desc") return "Golpea un árbol hasta conseguir madera";
            if (s == "achievement.buildWorkBench") return "Mesa de trabajo";
            if (s == "achievement.buildWorkBench.desc") return "Fabrica una mesa de trabajo con cuatro tablones";
            if (s == "achievement.buildPickaxe") return "Hora de minar";
            if (s == "achievement.buildPickaxe.desc") return "Fabrica un pico de madera";
            if (s == "achievement.buildFurnace") return "Horno caliente";
            if (s == "achievement.buildFurnace.desc") return "Fabrica un horno con ocho bloques de piedra";
            if (s == "achievement.acquireIron") return "Consigue hierro";
            if (s == "achievement.acquireIron.desc") return "Funde un lingote de hierro";
            if (s == "achievement.buildHoe") return "Hora de cultivar";
            if (s == "achievement.buildHoe.desc") return "Fabrica una azada de madera";
            if (s == "achievement.makeBread") return "Haz pan";
            if (s == "achievement.makeBread.desc") return "Cultiva trigo y conviértelo en pan";
            if (s == "achievement.bakeCake") return "La mentira";
            if (s == "achievement.bakeCake.desc") return "Hornea un pastel";
            if (s == "achievement.buildBetterPickaxe") return "Pico mejorado";
            if (s == "achievement.buildBetterPickaxe.desc") return "Fabrica un pico de piedra";
            if (s == "achievement.cookFish") return "Pescado sabroso";
            if (s == "achievement.cookFish.desc") return "Cocina un pez";
            if (s == "achievement.onARail") return "Sobre una vía";
            if (s == "achievement.onARail.desc") return "Viaja en vagoneta al menos 500 m en una sola dirección desde el punto de origen";
            if (s == "achievement.buildSword") return "Hora de luchar";
            if (s == "achievement.buildSword.desc") return "Fabrica una espada";
            if (s == "achievement.killEnemy") return "Cazador de monstruos";
            if (s == "achievement.killEnemy.desc") return "Caza un monstruo";
            if (s == "achievement.killCow") return "Cosecha de cuero";
            if (s == "achievement.killCow.desc") return "Recoge cuero";
            if (s == "achievement.flyPig") return "Cuando los cerdos vuelen";
            if (s == "achievement.flyPig.desc") return "Vuela desde una altura elevada montando un cerdo";
            if (s == "achievement.snipeSkeleton") return "Duelo de francotiradores";
            if (s == "achievement.snipeSkeleton.desc") return "Mata un esqueleto con un arco y flecha desde una distancia de 50 metros o más";
            if (s == "achievement.diamonds") return "¡Diamantes!";
            if (s == "achievement.diamonds.desc") return "Consigue diamantes";
            if (s == "achievement.portal") return "Necesitamos ir más profundo";
            if (s == "achievement.portal.desc") return "Construye, enciende y entra a un portal del Nether";
            if (s == "achievement.ghast") return "Devuélvelo";
            if (s == "achievement.ghast.desc") return "Destruye un ghast con su propia bola de fuego";
            if (s == "achievement.blazeRod") return "Al fuego";
            if (s == "achievement.blazeRod.desc") return "Alivia a un blaze de su vara";
            if (s == "achievement.potion") return "Cervecero local";
            if (s == "achievement.potion.desc") return "Prepara una poción";
            if (s == "achievement.theEnd") return "¿El Fin?";
            if (s == "achievement.theEnd.desc") return "Encuentra el Fin";
            if (s == "achievement.theEnd2") return "El Fin.";
            if (s == "achievement.theEnd2.desc") return "Derrota al Dragón";
            if (s == "achievement.enchantments") return "Encantador";
            if (s == "achievement.enchantments.desc") return "Construye una mesa de encantamientos";
            if (s == "achievement.overkill") return "Golpe letal";
            if (s == "achievement.overkill.desc") return "Inflige nueve corazones de daño con un solo golpe";
            if (s == "achievement.bookcase") return "Bibliotecario";
            if (s == "achievement.bookcase.desc") return "Construye estanterías para mejorar tu mesa de encantamientos";
        }
        return value;
    }
    if (s == "key.crafting")
    {
        if (isSpanish)
            return "Fabricar";
        return "Crafting";
    }
    // Game mode translations - Spanish fallbacks for selectWorld screen (when key not in table)
    if (isSpanish)
    {
        if (s == "selectWorld.gameMode.survival") return "Supervivencia";
        if (s == "selectWorld.gameMode.hardcore") return "Extremo";
        if (s == "selectWorld.gameMode.creative") return "Creativo";
        if (s == "selectWorld.gameMode.survival.line1") return "Minar, fabricar y sobrevivir";
        if (s == "selectWorld.gameMode.survival.line2") return "contra los monstruos de la noche";
        if (s == "selectWorld.gameMode.hardcore.line1") return "Una vida. Modo Extremo.";
        if (s == "selectWorld.gameMode.hardcore.line2") return "El mundo se borra al morir.";
        if (s == "selectWorld.gameMode.creative.line1") return "Recursos ilimitados. Sin salud ni hambre.";
        if (s == "selectWorld.gameMode.creative.line2") return "Vuela haciendo doble toque a Saltar.";
        // World type translations (when key not in table)
        if (s == "generator.default") return "Predeterminado";
        if (s == "generator.flat") return "Superplano";
        if (s == "generator.default_1_1") return "Predeterminado (1.1)";
        // --- Item name translations (Spanish) ---
        // Food
        if (s == "item.beefRaw.name") return "Bife crudo";
        if (s == "item.beefCooked.name") return "Bife";
        if (s == "item.porkchopRaw.name") return "Carne de cerdo cruda";
        if (s == "item.porkchopCooked.name") return "Carne de cerdo";
        if (s == "item.chickenRaw.name") return "Pollo crudo";
        if (s == "item.chickenCooked.name") return "Pollo cocinado";
        if (s == "item.fishRaw.name") return "Pescado crudo";
        if (s == "item.fishCooked.name") return "Pescado cocinado";
        if (s == "item.cookie.name") return "Galleta";
        if (s == "item.melon.name") return "Melón";
        if (s == "item.apple.name") return "Manzana";
        if (s == "item.appleGold.name") return "Manzana dorada";
        if (s == "item.bread.name") return "Pan";
        if (s == "item.mushroomStew.name") return "Estofado de champiñones";
        if (s == "item.bowl.name") return "Cuenco";
        if (s == "item.cake.name") return "Tarta";
        if (s == "item.sugar.name") return "Azúcar";
        if (s == "item.rottenFlesh.name") return "Carne podrida";
        if (s == "item.spiderEye.name") return "Ojo de araña";
        // Blocks (reporte: calabaza, linterna, tintes, etc.)
        if (s == "tile.pumpkin.name") return "Calabaza";
        if (s == "tile.litpumpkin.name") return "Linterna de calabaza";
        if (s == "tile.endPortal.name") return "Portal del End";
        if (s == "tile.endPortalFrame.name") return "Marco de portal del End";
        if (s == "tile.dragonEgg.name") return "Huevo de dragón";
        if (s == "tile.tnt.name") return "Dinamita";
        if (s == "tile.dispenser.name") return "Dispensador";
        if (s == "tile.notGate.name") return "Antorcha de redstone";
        if (s == "tile.stairsWood.name") return "Escaleras de madera";
        if (s == "tile.stairsStone.name") return "Escaleras de piedra";
        if (s == "tile.stairsBrick.name") return "Escaleras de ladrillo";
        if (s == "tile.stairsStoneBrickSmooth.name") return "Escaleras de ladrillo de piedra";
        if (s == "tile.stairsNetherBrick.name") return "Escaleras de ladrillo del Nether";
        if (s == "tile.fence.name") return "Cerca";
        if (s == "tile.fenceGate.name") return "Puerta de cerca";
        if (s == "tile.fenceIron.name") return "Barrotes de hierro";
        if (s == "tile.netherFence.name") return "Cerca del Nether";
        if (s == "tile.button.name") return "Botón";
        if (s == "tile.minecart.name") return "Vagoneta";
        // More items and blocks
        if (s == "item.seeds_pumpkin.name") return "Semillas de calabaza";
        if (s == "item.seeds_melon.name") return "Semillas de melón";
        if (s == "item.seeds.name") return "Semillas";
        if (s == "item.wheat.name") return "Trigo";
        if (s == "item.monsterPlacer.name") return "Generador";
        if (s == "item.monsterPlacer.ocelot.name") return "Generador de ocelote";
        if (s == "item.monsterPlacer.mooshroom.name") return "Generador de champivaca";
        if (s == "item.monsterPlacer.pigman.name") return "Generador de hombre cerdo zombi";
        if (s == "item.monsterPlacer.pig.name") return "Generador de cerdo";
        if (s == "item.monsterPlacer.cow.name") return "Generador de vaca";
        if (s == "item.monsterPlacer.sheep.name") return "Generador de oveja";
        if (s == "item.monsterPlacer.chicken.name") return "Generador de pollo";
        if (s == "item.monsterPlacer.wolf.name") return "Generador de lobo";
        if (s == "item.monsterPlacer.villager.name") return "Generador de aldeano";
        if (s == "item.monsterPlacer.squid.name") return "Generador de calamar";
        if (s == "item.monsterPlacer.creeper.name") return "Generador de creeper";
        if (s == "item.monsterPlacer.skeleton.name") return "Generador de esqueleto";
        if (s == "item.monsterPlacer.zombie.name") return "Generador de zombi";
        if (s == "item.monsterPlacer.spider.name") return "Generador de araña";
        if (s == "item.monsterPlacer.slime.name") return "Generador de slime";
        if (s == "item.monsterPlacer.ghast.name") return "Generador de ghast";
        if (s == "item.monsterPlacer.enderman.name") return "Generador de enderman";
        if (s == "item.monsterPlacer.silverfish.name") return "Generador de pez plata";
        if (s == "item.monsterPlacer.caveSpider.name") return "Generador de araña de caverna";
        if (s == "item.monsterPlacer.blaze.name") return "Generador de blaze";
        if (s == "item.monsterPlacer.lavaSlime.name") return "Generador de cubo de magma";
        // Potion splash prefix
        if (s == "potion.prefix.splash.name") return "arrojadiza";
        // Dye names
        if (s == "item.dyePowder.red.name") return "Tinte rojo";
        if (s == "item.dyePowder.green.name") return "Tinte verde";
        if (s == "item.dyePowder.yellow.name") return "Tinte amarillo";
        if (s == "item.dyePowder.blue.name") return "Tinte azul";
        if (s == "item.dyePowder.purple.name") return "Tinte morado";
        if (s == "item.dyePowder.cyan.name") return "Tinte cian";
        if (s == "item.dyePowder.black.name") return "Tinte negro";
        if (s == "item.dyePowder.brown.name") return "Tinte marrón";
        if (s == "item.dyePowder.silver.name") return "Tinte gris claro";
        if (s == "item.dyePowder.gray.name") return "Tinte gris";
        if (s == "item.dyePowder.pink.name") return "Tinte rosa";
        if (s == "item.dyePowder.lime.name") return "Tinte lima";
        if (s == "item.dyePowder.lightBlue.name") return "Tinte azul claro";
        if (s == "item.dyePowder.magenta.name") return "Tinte magenta";
        if (s == "item.dyePowder.orange.name") return "Tinte naranja";
        if (s == "item.dyePowder.white.name") return "Tinte blanco";
        // More blocks (stairs, fences, buttons from report)
        if (s == "tile.wood.oak.name") return "Madera de roble";
        if (s == "tile.wood.spruce.name") return "Madera de abeto";
        if (s == "tile.wood.birch.name") return "Madera de abedul";
        if (s == "tile.wood.jungle.name") return "Madera de jungla";
        if (s == "tile.leaves.oak.name") return "Hojas de roble";
        if (s == "tile.leaves.spruce.name") return "Hojas de abeto";
        if (s == "tile.leaves.birch.name") return "Hojas de abedul";
        if (s == "tile.leaves.jungle.name") return "Hojas de jungla";
        if (s == "tile.sapling.oak.name") return "Brote de roble";
        if (s == "tile.sapling.spruce.name") return "Brote de abeto";
        if (s == "tile.sapling.birch.name") return "Brote de abedul";
        if (s == "tile.sapling.jungle.name") return "Brote de jungla";
        if (s == "tile.planks.oak.name") return "Tablas de roble";
        if (s == "tile.planks.spruce.name") return "Tablas de abeto";
        if (s == "tile.planks.birch.name") return "Tablas de abedul";
        if (s == "tile.planks.jungle.name") return "Tablas de jungla";
        if (s == "tile.stonebricksmooth.name") return "Ladrillo de piedra";
        if (s == "tile.stonebricksmooth.mossy.name") return "Ladrillo de piedra musgoso";
        if (s == "tile.stonebricksmooth.cracked.name") return "Ladrillo de piedra agrietado";
        if (s == "tile.stonebricksmooth.chiseled.name") return "Ladrillo de piedra tallado";
        if (s == "tile.sandStone.chiseled.name") return "Arenisca tallada";
        if (s == "tile.sandStone.smooth.name") return "Arenisca lisa";
        if (s == "tile.mushroom.brown.name") return "Champiñón marrón";
        if (s == "tile.mushroom.red.name") return "Champiñón rojo";
        if (s == "item.charcoal.name") return "Carbón vegetal";
        if (s == "item.coal.name") return "Carbón";
        if (s == "item.doorWood.name") return "Puerta de madera";
        if (s == "item.doorIron.name") return "Puerta de hierro";
        if (s == "item.swordWood.name") return "Espada de madera";
        if (s == "item.shovelWood.name") return "Pala de madera";
        if (s == "item.pickaxeWood.name") return "Pico de madera";
        if (s == "item.hatchetWood.name") return "Hacha de madera";
        if (s == "item.hoeWood.name") return "Azada de madera";
        if (s == "item.shovelStone.name") return "Pala de piedra";
        if (s == "item.pickaxeStone.name") return "Pico de piedra";
        if (s == "item.hatchetStone.name") return "Hacha de piedra";
        if (s == "item.hoeStone.name") return "Azada de piedra";
        if (s == "item.swordStone.name") return "Espada de piedra";
        if (s == "item.shovelIron.name") return "Pala de hierro";
        if (s == "item.pickaxeIron.name") return "Pico de hierro";
        if (s == "item.hatchetIron.name") return "Hacha de hierro";
        if (s == "item.hoeIron.name") return "Azada de hierro";
        if (s == "item.swordIron.name") return "Espada de hierro";
        if (s == "item.shovelDiamond.name") return "Pala de diamante";
        if (s == "item.pickaxeDiamond.name") return "Pico de diamante";
        if (s == "item.hatchetDiamond.name") return "Hacha de diamante";
        if (s == "item.hoeDiamond.name") return "Azada de diamante";
        if (s == "item.swordDiamond.name") return "Espada de diamante";
        if (s == "item.shovelGold.name") return "Pala de oro";
        if (s == "item.pickaxeGold.name") return "Pico de oro";
        if (s == "item.hatchetGold.name") return "Hacha de oro";
        if (s == "item.hoeGold.name") return "Azada de oro";
        if (s == "item.swordGold.name") return "Espada de oro";
        if (s == "item.stick.name") return "Palo";
        if (s == "item.arrow.name") return "Flecha";
        if (s == "item.bow.name") return "Arco";
        if (s == "item.flintAndSteel.name") return "Mechero";
        if (s == "item.flint.name") return "Pedernal";
        if (s == "item.feather.name") return "Pluma";
        if (s == "item.string.name") return "Hilo";
        if (s == "item.sulphur.name") return "Pólvora";
        if (s == "item.gunpowder.name") return "Pólvora";
        if (s == "item.leather.name") return "Cuero";
        if (s == "item.bootsCloth.name") return "Botas de cuero";
        if (s == "item.leggingsCloth.name") return "Pantalones de cuero";
        if (s == "item.chestplateCloth.name") return "Peto de cuero";
        if (s == "item.helmetCloth.name") return "Casco de cuero";
        if (s == "item.bucket.name") return "Cubo";
        if (s == "item.bucketWater.name") return "Cubo de agua";
        if (s == "item.bucketLava.name") return "Cubo de lava";
        if (s == "item.milk.name") return "Leche";
        if (s == "item.painting.name") return "Pintura";
        if (s == "item.sign.name") return "Cartel";
        if (s == "item.saddle.name") return "Silla";
        if (s == "item.bone.name") return "Hueso";
        if (s == "item.slimeball.name") return "Bola de slime";
        if (s == "item.egg.name") return "Huevo";
        if (s == "item.redstone.name") return "Redstone";
        if (s == "item.glowstone.name") return "Piedra luminosa";
        if (s == "item.compass.name") return "Brújula";
        if (s == "item.clock.name") return "Reloj";
        if (s == "item.fishingRod.name") return "Caña de pescar";
        if (s == "item.emptyMap.name") return "Mapa vacío";
        if (s == "item.map.name") return "Mapa";
        if (s == "item.book.name") return "Libro";
        if (s == "item.paper.name") return "Papel";
        if (s == "item.reeds.name") return "Caña de azúcar";
        if (s == "item.clay.name") return "Arcilla";
        if (s == "item.brick.name") return "Ladrillo";
        if (s == "item.emerald.name") return "Diamante";
        if (s == "item.ingotIron.name") return "Lingote de hierro";
        if (s == "item.ingotGold.name") return "Lingote de oro";
        if (s == "item.netherStalkSeeds.name") return "Semillas de verrugas del Nether";
        if (s == "item.blazeRod.name") return "Vara de blaze";
        if (s == "item.blazePowder.name") return "Polvo de blaze";
        if (s == "item.ghastTear.name") return "Lágrima de ghast";
        if (s == "item.enderPearl.name") return "Perla de ender";
        if (s == "item.eyeOfEnder.name") return "Ojo de ender";
        if (s == "item.fireball.name") return "Carga de fuego";
        if (s == "item.netherbrick.name") return "Ladrillo del Nether";
        if (s == "item.netherrack.name") return "Netherrack";
        if (s == "item.glassBottle.name") return "Botella de vidrio";
        if (s == "item.potion.name") return "Poción";
        if (s == "item.cauldron.name") return "Caldero";
        if (s == "item.brewingStand.name") return "Soporte de pociones";
        if (s == "tile.enchantmentTable.name") return "Mesa de encantamientos";
        if (s == "tile.brewingStand.name") return "Soporte de pociones";
        if (s == "tile.cauldron.name") return "Caldero";
        if (s == "tile.whiteStone.name") return "Piedra del End";
        if (s == "tile.netherBrick.name") return "Ladrillo del Nether";
        if (s == "tile.mycel.name") return "Micelio";
        if (s == "tile.waterlily.name") return "Lirio acuático";
        if (s == "tile.vine.name") return "Enredadera";
        if (s == "tile.melon.name") return "Melón";
        if (s == "tile.cactus.name") return "Cactus";
        if (s == "tile.pumpkinStem.name") return "Tallo de calabaza";
        if (s == "tile.melonStem.name") return "Tallo de melón";
    }
    return s;
}

std::string StringTranslate::translateKeyFormat(const std::string &s, const std::vector<std::string> &args)
{
    std::string result = translateKey(s);
    std::size_t sequentialArg = 0;
    std::size_t searchFrom = 0;
    while (sequentialArg < args.size())
    {
        std::size_t pos = result.find("%s", searchFrom);
        if (pos == std::string::npos)
            break;
        result.replace(pos, 2, args[sequentialArg]);
        searchFrom = pos + args[sequentialArg].length();
        ++sequentialArg;
    }

    for (std::size_t i = 0; i < args.size(); ++i)
    {
        std::string token = "%" + std::to_string(i + 1) + "$s";
        std::size_t pos = 0;
        while ((pos = result.find(token, pos)) != std::string::npos)
        {
            result.replace(pos, token.length(), args[i]);
            pos += args[i].length();
        }
    }
    return result;
}

std::string StringTranslate::translateKeyFormat(const std::string &s, const std::string &arg)
{
    return translateKeyFormat(s, std::vector<std::string>{arg});
}

std::string StringTranslate::translateKeyFormat(const std::string &s, const char *arg)
{
    return translateKeyFormat(s, std::vector<std::string>{arg != nullptr ? arg : ""});
}

std::string StringTranslate::translateNamedKey(const std::string &s)
{
    auto it = translateTable.find(s + ".name");
    if (it != translateTable.end())
        return it->second;
    // Hardcoded Spanish fallbacks for item names missing from lang files
    bool isSpanish = false;
    if (currentLanguage.size() >= 3)
    {
        char c0 = currentLanguage[0];
        char c1 = currentLanguage[1];
        char c2 = currentLanguage[2];
        if ((c0 == 'e' || c0 == 'E') && (c1 == 's' || c1 == 'S') && (c2 == '_' || c2 == '-'))
            isSpanish = true;
    }
    if (isSpanish)
    {
        if (s == "item.beefRaw") return "Filete de vaca crudo";
        if (s == "item.beefCooked") return "Filete de vaca";
        if (s == "item.porkRaw") return "Filete de cerdo crudo";
        if (s == "item.porkCooked") return "Filete de cerdo";
        if (s == "item.chickenRaw") return "Pollo crudo";
        if (s == "item.chickenCooked") return "Pollo cocinado";
        if (s == "item.fishRaw") return "Pescado crudo";
        if (s == "item.fishCooked") return "Pescado cocinado";
        // Dyes
        if (s == "item.dyePowder") return "Tinte";
        if (s == "item.dyePowder.black") return "Tinte negro";
        if (s == "item.dyePowder.red") return "Tinte rojo";
        if (s == "item.dyePowder.green") return "Tinte verde";
        if (s == "item.dyePowder.brown") return "Tinte marrón";
        if (s == "item.dyePowder.blue") return "Tinte azul";
        if (s == "item.dyePowder.purple") return "Tinte morado";
        if (s == "item.dyePowder.cyan") return "Tinte cian";
        if (s == "item.dyePowder.silver") return "Tinte gris claro";
        if (s == "item.dyePowder.gray") return "Tinte gris";
        if (s == "item.dyePowder.pink") return "Tinte rosa";
        if (s == "item.dyePowder.lime") return "Tinte lima";
        if (s == "item.dyePowder.yellow") return "Tinte amarillo";
        if (s == "item.dyePowder.lightBlue") return "Tinte azul claro";
        if (s == "item.dyePowder.magenta") return "Tinte magenta";
        if (s == "item.dyePowder.orange") return "Tinte naranja";
        if (s == "item.dyePowder.white") return "Tinte blanco";
        // Blocks with metadata variants - stone bricks
        if (s == "tile.stoneBrick") return "Ladrillo de piedra";
        if (s == "tile.stoneBrick.mossy") return "Ladrillo de piedra musgoso";
        if (s == "tile.stoneBrick.cracked") return "Ladrillo de piedra agrietado";
        if (s == "tile.stoneBrick.chiseled") return "Ladrillo de piedra tallado";
        // Wood/planks variants
        if (s == "tile.wood.oak") return "Tronco de roble";
        if (s == "tile.wood.spruce") return "Tronco de abeto";
        if (s == "tile.wood.birch") return "Tronco de abedul";
        if (s == "tile.wood.jungle") return "Tronco de jungla";
        if (s == "tile.planks.oak") return "Tablas de roble";
        if (s == "tile.planks.spruce") return "Tablas de abeto";
        if (s == "tile.planks.birch") return "Tablas de abedul";
        if (s == "tile.planks.jungle") return "Tablas de jungla";
        // Sandstone variants
        if (s == "tile.sandStone") return "Arenisca";
        if (s == "tile.sandStone.chiseled") return "Arenisca tallada";
        if (s == "tile.sandStone.smooth") return "Arenisca lisa";
        // Leaves variants
        if (s == "tile.leaves.oak") return "Hojas de roble";
        if (s == "tile.leaves.spruce") return "Hojas de abeto";
        if (s == "tile.leaves.birch") return "Hojas de abedul";
        if (s == "tile.leaves.jungle") return "Hojas de jungla";
        // Saplings
        if (s == "tile.sapling.oak") return "Brote de roble";
        if (s == "tile.sapling.spruce") return "Brote de abeto";
        if (s == "tile.sapling.birch") return "Brote de abedul";
        if (s == "tile.sapling.jungle") return "Brote de jungla";
        // Mushrooms
        if (s == "tile.mushroomBrown") return "Seta marrón";
        if (s == "tile.mushroomRed") return "Seta roja";
        // Pumpkin/melon
        if (s == "tile.pumpkin") return "Calabaza";
        if (s == "tile.pumpkinLantern") return "Linterna de calabaza";
        if (s == "tile.melon") return "Melón";
        // Flowers
        if (s == "tile.plantYellow") return "Diente de león";
        if (s == "tile.plantRed") return "Rosa";
        // TNT, Dispenser, etc.
        if (s == "tile.tnt") return "TNT";
        if (s == "tile.dispenser") return "Dispensador";
        if (s == "tile.torchRedstone") return "Antorcha de redstone";
        if (s == "tile.minecart") return "Vagoneta";
        // Bowls
        if (s == "item.bowl") return "Cuenco";
        // Seeds
        if (s == "item.seedsPumpkin") return "Semillas de calabaza";
        if (s == "item.seedsMelon") return "Semillas de melón";
        // Spawn eggs
        if (s == "item.monsterPlacer.ocelot") return "Generador de ocelote";
        // Mob names
        if (s == "entity.Mooshroom.name") return "Champivaca";
        if (s == "entity.PigZombie.name") return "Cerdo zombi";
        if (s == "entity.Pig.name") return "Cerdo";
        // Potion prefix
        if (s == "potion.prefix.splash") return "Poción arrojadiza";
    }
    return "";
}

bool StringTranslate::loadLanguageFile(const std::string &path, bool ui)
{
    std::unique_ptr<std::istream> owned = openLanguageResource(path);
    std::istream *input = owned.get();
    if (input == nullptr || !(*input))
    {
#ifdef PS2_PLATFORM
        MC_LOG_DEBUG("ps2", "language missing: %s\n", path.c_str());
#endif
        return false;
    }

    std::string line;
    while (std::getline(*input, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        line = trim(line);
        if (line.empty() || line[0] == '#')
            continue;
        std::size_t equals = line.find('=');
        if (equals == std::string::npos)
            continue;
        const std::string key = trim(line.substr(0, equals));
        const std::string value = trim(line.substr(equals + 1));
        // Empty overrides must not erase the English fallback.
        if (!key.empty() && !value.empty())
            translateTable[(ui ? "ui." : "") + key] = value;
    }
    return true;
}

std::string StringTranslate::trim(const std::string &s)
{
    std::size_t first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return "";
    std::size_t last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

void StringTranslate::updateUnicodeFlag()
{
    unicode = false;
    for (const auto &entry : translateTable)
    {
        if (hasCodepointAtLeast256(entry.second))
        {
            unicode = true;
            break;
        }
    }
}
