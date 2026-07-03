# [barista-fork] Proactive voiced barista assistant — self-contained private-fork module.
#
# Included ONCE from CMakeLists.txt, after the Decenza target + its qml module are defined.
# Everything the feature needs (C++ sources, QML resource, compile flag) is registered here,
# so the upstream source/QML lists stay untouched and upstream `main` keeps merging cleanly.
#
# Toggle the whole feature off (e.g. to bisect an upstream regression against a vanilla build):
#   cmake -DDECENZA_BARISTA=OFF ...

option(DECENZA_BARISTA "Build the proactive barista assistant module" ON)

# CoachingCard is a hygiene EXTRACTION of an existing fork feature (the post-shot coaching card),
# NOT part of the new assistant — so it is registered UNCONDITIONALLY. A DECENZA_BARISTA=OFF build
# still shows the card (PostShotReviewPage's Loader loads this resource); only the NEW assistant
# (C++ module + overlay) is gated by the flag, so OFF is a clean "everything-but-the-assistant" bisect build.
# BASE strips the on-disk path so files land at :/qml/assistant/<File>.qml.
qt_add_resources(Decenza "barista_extracted_qml"
    PREFIX "/qml/assistant"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/qml/assistant"
    FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/qml/assistant/CoachingCard.qml
)

if(DECENZA_BARISTA)
    target_sources(Decenza PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/barista/baristamodule.h
        ${CMAKE_CURRENT_SOURCE_DIR}/src/barista/baristamodule.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/barista/assistantsettings.h
        ${CMAKE_CURRENT_SOURCE_DIR}/src/barista/assistantsettings.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/barista/assistantorchestrator.h
        ${CMAKE_CURRENT_SOURCE_DIR}/src/barista/assistantorchestrator.cpp
    )

    # New-assistant QML — its own resource, loaded by URL via a Loader in main.qml.
    qt_add_resources(Decenza "barista_qml"
        PREFIX "/qml/assistant"
        BASE "${CMAKE_CURRENT_SOURCE_DIR}/qml/assistant"
        FILES
            ${CMAKE_CURRENT_SOURCE_DIR}/qml/assistant/AssistantOverlay.qml
    )

    target_compile_definitions(Decenza PRIVATE DECENZA_BARISTA=1)
endif()
