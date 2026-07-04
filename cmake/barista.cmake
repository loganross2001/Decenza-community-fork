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
        ${CMAKE_CURRENT_SOURCE_DIR}/src/barista/assistantvoice.h
        ${CMAKE_CURRENT_SOURCE_DIR}/src/barista/assistantvoice.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/barista/voiceinput.h
        ${CMAKE_CURRENT_SOURCE_DIR}/src/barista/voiceinput.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/barista/baristaknowledge.h
        ${CMAKE_CURRENT_SOURCE_DIR}/src/barista/baristaknowledge.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/barista/baristaactions.h
        ${CMAKE_CURRENT_SOURCE_DIR}/src/barista/baristaactions.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/barista/baristacontextbuilder.h
        ${CMAKE_CURRENT_SOURCE_DIR}/src/barista/baristacontextbuilder.cpp
    )

    # New-assistant QML — its own resource, loaded by URL via a Loader in main.qml.
    qt_add_resources(Decenza "barista_qml"
        PREFIX "/qml/assistant"
        BASE "${CMAKE_CURRENT_SOURCE_DIR}/qml/assistant"
        FILES
            ${CMAKE_CURRENT_SOURCE_DIR}/qml/assistant/AssistantOverlay.qml
            ${CMAKE_CURRENT_SOURCE_DIR}/qml/assistant/AssistantSettingsPanel.qml
            ${CMAKE_CURRENT_SOURCE_DIR}/qml/assistant/AssistantSettingsSection.qml
            ${CMAKE_CURRENT_SOURCE_DIR}/qml/assistant/ActionConfirmChip.qml
            ${CMAKE_CURRENT_SOURCE_DIR}/qml/assistant/BaristaAvatar.qml
    )

    target_compile_definitions(Decenza PRIVATE DECENZA_BARISTA=1)
endif()
