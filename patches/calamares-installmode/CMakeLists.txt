if(NOT WITH_QML)
    calamares_skip_module( "installmode (QML is not supported in this build)" )
    return()
endif()

calamares_add_plugin(installmode
    TYPE viewmodule
    EXPORT_MACRO PLUGINDLLEXPORT_PRO
    SOURCES
        InstallModeViewStep.cpp
    SHARED_LIB
)
