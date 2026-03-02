/^source=/{sub(/\)$/,"\n        \"fix-struts-menu-position.patch\")")}
/^sha256sums=/{sub(/\)$/,"\n            'SKIP')")}
/^build\(\) \{$/{print "prepare() {\n  cd \"${pkgname}\"\n  patch -Np1 -i \"${srcdir}/fix-struts-menu-position.patch\"\n}\n"}
1
