# Custom libvdpau cmake file

find_package( PkgConfig )
if( PKG_CONFIG_FOUND )
    pkg_search_module( PC_LIBVDPAU vdpau )
endif( PKG_CONFIG_FOUND )

find_path( LIBVDPAU_INCLUDE_DIR vdpau/vdpau.h
    HINTS
        # Hints provided by pkg-config
        ${PC_LIBVDPAU_INCLUDEDIR}
        ${PC_LIBVDPAU_INCLUDEDIR}/*
        ${PC_LIBVDPAU_INCLUDE_DIRS}
    PATHS
        # Standard include directories
        /usr/include/
        ~/usr/include/
        /opt/local/include/
        /usr/local/include/
        /opt/kde4/include/
        ${KDE4_INCLUDE_DIR}/
        # Search all subdirs of the above
        /usr/include/*
        ~/usr/include/*
        /opt/local/include/*
        /usr/local/include/*
        /opt/kde4/include/*
        ${KDE4_INCLUDE_DIR}/*
    PATH_SUFFIXES
        # Subdirectory hints
        libvdpau
        vdpau
)

find_library( LIBVDPAU_LIBRARY vdpau
    HINTS
        # Hints provided by pkg-config
        ${PC_LIBVDPAU_LIBDIR}
        ${PC_LIBVDPAU_LIBRARY_DIRS}
    PATHS
        ~/usr/lib/
        /opt/local/lib/
        /usr/lib/
        /usr/lib64/
        /usr/local/lib/
        /opt/kde4/lib/
        ${KDE4_LIB_DIR}
)

include( FindPackageHandleStandardArgs )
find_package_handle_standard_args( vdpau DEFAULT_MSG
    LIBVDPAU_INCLUDE_DIR
    LIBVDPAU_LIBRARY
)
if( LIBVDPAU_FOUND )
    message( STATUS "\tvdpau: ${LIBVDPAU_INCLUDE_DIR}, ${LIBVDPAU_LIBRARY}" )
endif( LIBVDPAU_FOUND )

mark_as_advanced( LIBVDPAU_LIBRARY )
