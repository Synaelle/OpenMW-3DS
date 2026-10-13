#!/usr/bin/env bash
# Attributes .text/.rodata size of an ELF to rough buckets (namespace / library prefixes).
#   scripts/size-report.sh build/openmw/openmw.elf
elf="$1"
/opt/devkitpro/devkitARM/bin/arm-none-eabi-nm -C --size-sort -S "$elf" 2>/dev/null \
  | awk '$3 ~ /^[tTrR]$/ { size = strtonum("0x" $2); $1=$2=$3=""; name=substr($0,4); print size "\t" name }' \
  | awk -F'\t' '
    function bucket(n) {
        if (n ~ /^sol::|sol::/) return "sol3 (Lua bindings templates)";
        if (n ~ /^MWLua::/) return "MWLua (Lua bindings)";
        if (n ~ /^osgDB::|^osgdb_|ReaderWriter|serializer|Serializer|Wrapper/) return "OSG plugins/serializers";
        if (n ~ /^osg::|^osgUtil::|^osgAnimation::|^osgParticle::|^osgText::|^osgViewer::|^osgGA::|^osgShadow::|^osgSim::|^OpenThreads::|^osgFX::/) return "OSG core";
        if (n ~ /^icu_73::|^u[a-z]*_[0-9a-zA-Z]*_73|^u[a-z]*_73/) return "ICU";
        if (n ~ /^MyGUI::/) return "MyGUI";
        if (n ~ /^MWGui::/) return "MWGui";
        if (n ~ /^MWRender::/) return "MWRender";
        if (n ~ /^MWMechanics::/) return "MWMechanics";
        if (n ~ /^MWWorld::/) return "MWWorld";
        if (n ~ /^MWClass::/) return "MWClass";
        if (n ~ /^MWScript::|^Interpreter::|^Compiler::/) return "MWScript/compiler";
        if (n ~ /^ESM4::/) return "ESM4 (Oblivion/Skyrim formats)";
        if (n ~ /^ESM::/) return "ESM (Morrowind formats)";
        if (n ~ /^DetourNavigator::|^dt|^rc|Recast|Detour/) return "Navigator/Recast";
        if (n ~ /^bt|Bullet/) return "Bullet";
        if (n ~ /^Nif::|^NifOsg::|^NifBullet::/) return "NIF";
        if (n ~ /^SceneUtil::|^Shader::|^Terrain::|^Resource::|^fx::|^Fx::/) return "SceneUtil/Shader/Terrain/Resource/fx";
        if (n ~ /^MWPhysics::/) return "MWPhysics";
        if (n ~ /^MWSound::|^MWInput::|^MWDialogue::|^MWState::|^MWBase::/) return "MW sound/input/dialogue/state";
        if (n ~ /^LuaUi::|^LuaUtil::/) return "components/lua";
        if (n ~ /^Settings::/) return "Settings";
        if (n ~ /^YAML::/) return "yaml-cpp";
        if (n ~ /^sqlite3|^sqlite/) return "SQLite";
        if (n ~ /^SDL_|^SDL/) return "SDL2";
        if (n ~ /^lua|^luaL_|^luaV|^luaH|^luaK|^luaY|^luaX|^luaD|^luaG|^luaC|^luaF|^luaS|^luaT|^luaO|^luaU|^luaZ|^luaE|^luaM|^luaP/) return "Lua VM";
        if (n ~ /^boost::/) return "Boost";
        if (n ~ /^std::|^__gnu|^__cxx|^void std::|operator/) return "libstdc++/templates";
        if (n ~ /^FT_|^ft_|^png_|^jpeg_|^inflate|^deflate|^BZ2_|^LZ4/) return "freetype/png/jpeg/zlib";
        return "other";
    }
    { s[bucket($2)] += $1; total += $1 }
    END { for (b in s) printf "%8.2f MB  %5.1f%%  %s\n", s[b]/1048576, 100*s[b]/total, b; printf "%8.2f MB  total (symbols)\n", total/1048576 }' \
  | sort -rn
