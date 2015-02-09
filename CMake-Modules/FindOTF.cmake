IF (NOT DEFINED OTF_FOUND)
    SET (OTF_FOUND FALSE)
ENDIF()

FIND_PATH(OTF_INCLUDE_DIR otf.h
    ${HOME}/opt/include
    ${HOME}/opt/include/otf
    ${HOME}/opt/include/open-trace-format
    /usr/include
)

FIND_LIBRARY(OTF_LIBRARIES otf
    ${HOME}/opt/lib
    /usr/lib
)

IF (NOT OTF_LIBRARIES)
    FIND_LIBRARY(OTF_LIBRARIES open-trace-format
	${HOME}/opt/lib
	/usr/lib
    )
ENDIF()

IF (OTF_INCLUDE_DIR AND OTF_LIBRARIES)
    SET(OTF_FOUND TRUE)
    IF (CMAKE_VERBOSE_MAKEFILE)
	MESSAGE("Using OTF_INCLUDE_DIR = " ${OTF_INCLUDE_DIR})
	MESSAGE("Using OTF_LIBRARIES = " ${OTF_LIBRARIES})
    ENDIF()
ENDIF()
