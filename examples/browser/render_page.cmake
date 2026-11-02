# Bind the document's UI scripts and stylesheet to their actual content. A
# returning visitor must not combine new HTML with a cached, incompatible UI.
file(READ "${SOURCE}" page)
string(REGEX MATCHALL "(<script[^>]*src|<link[^>]*href)=\"[^\"?#]+\\.(js|css)\"" entries "${page}")
foreach(entry IN LISTS entries)
    string(REGEX REPLACE "^.*=\"([^\"]+)\"$" "\\1" asset "${entry}")
    if(NOT EXISTS "${DIST}/${asset}")
        # A partial local build may omit QuickJS when guest tools are missing.
        # Complete release builds separately require every benchmark asset.
        continue()
    endif()
    file(SHA256 "${DIST}/${asset}" hash)
    string(REPLACE "\"${asset}\"" "\"${asset}?v=${hash}\"" versioned "${entry}")
    string(REPLACE "${entry}" "${versioned}" page "${page}")
endforeach()
file(WRITE "${DIST}/index.html.tmp" "${page}")
configure_file("${DIST}/index.html.tmp" "${DIST}/index.html" COPYONLY)
file(REMOVE "${DIST}/index.html.tmp")
