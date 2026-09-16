# Checks whether Aroma's plugin loader would read the wrong section as a loaded
# section's relocations, and so refuse the plugin. Reads what readelf prints for a
# finished .wps:
#
#   powerpc-eabi-readelf -S -W -t JustGetMiiOnline.wps | awk -f relocation-pairing.awk
#
# WiiUPluginLoaderBackend links every loaded section, PROGBITS or NOBITS with the alloc
# flag, by taking the first section whose sh_info equals the loaded section's index and
# reading it as relocations. It never checks that what it took is a relocation table.
# A symbol table's sh_info is its count of local symbols, so when that count equals the
# index of a loaded section, the loader reads the symbol table as that section's
# relocations, can't find a symbol, and prints "Failed to load plugin" before any hook
# runs. The code is linkSection in PluginLinkInformationFactory.cpp.
#
# This replays that pairing and nothing else the loader does. It exits 1 naming the
# pair when a loaded section would be linked with anything but REL or RELA, and 2 when
# it read no section table at all, so a readelf that printed nothing never passes.
#
# The names readelf prints for a .wps are garbage, because an RPL compresses its section
# name table. This goes by index, type, sh_info and flags, which are stored plain.

/^  \[ *[0-9]+\]/ {
    number = $0
    sub(/^  \[ */, "", number)
    sub(/\].*/, "", number)
    section = number + 0
    if (section + 1 > count)
        count = section + 1
    expect = "fields"
    next
}

# readelf -t puts the type, addresses, sizes, link, info and alignment on the line after
# the index, and the flags spelled out on the line after that.
expect == "fields" {
    type[section] = $1
    info[section] = $7 + 0
    expect = "flags"
    next
}

expect == "flags" {
    alloc[section] = (index($0, "ALLOC") > 0)
    expect = ""
    next
}

END {
    if (count == 0) {
        print "no section table read"
        exit 2
    }
    refused = 0
    for (loaded = 1; loaded < count; loaded++) {
        if (!alloc[loaded] || (type[loaded] != "PROGBITS" && type[loaded] != "NOBITS"))
            continue
        for (other = 0; other < count; other++) {
            if (!(other in info) || info[other] != loaded)
                continue
            if (type[other] != "RELA" && type[other] != "REL") {
                printf "section %d (%s) would have section %d (%s) read as its relocations\n", loaded, type[loaded], other, type[other]
                refused = 1
            }
            break
        }
    }
    exit refused
}
