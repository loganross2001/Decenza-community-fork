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

# BaristaTools is the extracted home of the barista's private client-side AI tool definitions + executor
# (query_shots, get_shot_detail, compare_shots, get_bean_profile, detect_grind_drift). It is registered
# UNCONDITIONALLY — the shared, always-compiled aimanager.cpp references BaristaTools::toolDefinitions()/
# executeTool(), so a DECENZA_BARISTA=OFF build must still get the definition or it fails to link. The module
# depends only on upstream files (shothistorystorage.h, shotsummarizer.h, dbutils.h), so it compiles standalone.
target_sources(Decenza PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/barista/baristatools.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/barista/baristatools.cpp
)

# FeedbackStorage owns the barista's verbal-feedback KB (separate assistant.db). Registered
# UNCONDITIONALLY like BaristaTools: baristatools.cpp (unconditional) references FeedbackStorage
# symbols in the log_tasting_feedback/search_tasting_feedback executors, and aimanager.cpp reads
# it for the proactive bean-feedback context block, so a DECENZA_BARISTA=OFF build must still link.
target_sources(Decenza PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/barista/feedbackstorage.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/barista/feedbackstorage.cpp
)

# TasksStorage owns the barista's reminders + maintenance tracking (SAME assistant.db as FeedbackStorage).
# Registered UNCONDITIONALLY like FeedbackStorage: baristatools.cpp (unconditional) references TasksStorage
# in the create_reminder/list_due_reminders/complete_reminder/log_maintenance executors, and aimanager.cpp
# reads it for the proactive dueItems context block, so a DECENZA_BARISTA=OFF build must still link.
target_sources(Decenza PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/barista/tasksstorage.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/barista/tasksstorage.cpp
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
        # [barista-fork] Periodic Decent maintenance-docs check (network + rate-limit + hash). Gated with
        # the module — nothing unconditional references it (the DB state/apply helpers live in the
        # unconditional TasksStorage), so it must NOT be built into a DECENZA_BARISTA=OFF binary or the
        # DB-only tests (it pulls in QtNetwork).
        ${CMAKE_CURRENT_SOURCE_DIR}/src/barista/maintenancedocsync.h
        ${CMAKE_CURRENT_SOURCE_DIR}/src/barista/maintenancedocsync.cpp
        # [barista-fork] Fast-path web tools (get_weather / get_stock_quote / get_local_news). Gated WITH the
        # module — only baristamodule.cpp (also gated) names BaristaWebTools; baristatools.cpp reaches the tools
        # through a std::function seam, so this stays out of the DB-only tests (it pulls in QtNetwork).
        ${CMAKE_CURRENT_SOURCE_DIR}/src/barista/baristawebtools.h
        ${CMAKE_CURRENT_SOURCE_DIR}/src/barista/baristawebtools.cpp
    )

    # New-assistant QML — its own resource, loaded by URL via a Loader in main.qml.
    qt_add_resources(Decenza "barista_qml"
        PREFIX "/qml/assistant"
        BASE "${CMAKE_CURRENT_SOURCE_DIR}/qml/assistant"
        FILES
            ${CMAKE_CURRENT_SOURCE_DIR}/qml/assistant/AssistantOverlay.qml
            ${CMAKE_CURRENT_SOURCE_DIR}/qml/assistant/AssistantSettingsPanel.qml
            ${CMAKE_CURRENT_SOURCE_DIR}/qml/assistant/AssistantSettingsSection.qml
            ${CMAKE_CURRENT_SOURCE_DIR}/qml/assistant/BaristaSavedVoices.qml
            ${CMAKE_CURRENT_SOURCE_DIR}/qml/assistant/ElevenLabsVoicePicker.qml
            ${CMAKE_CURRENT_SOURCE_DIR}/qml/assistant/MaintenanceSettingsDialog.qml

            ${CMAKE_CURRENT_SOURCE_DIR}/qml/assistant/ActionConfirmChip.qml
            ${CMAKE_CURRENT_SOURCE_DIR}/qml/assistant/BaristaAvatar.qml
            ${CMAKE_CURRENT_SOURCE_DIR}/qml/assistant/avatars/AvatarFace.qml
            ${CMAKE_CURRENT_SOURCE_DIR}/qml/assistant/avatars/AvatarCup.qml
            ${CMAKE_CURRENT_SOURCE_DIR}/qml/assistant/avatars/AvatarOrb.qml
            ${CMAKE_CURRENT_SOURCE_DIR}/qml/assistant/avatars/AvatarBean.qml
    )

    target_compile_definitions(Decenza PRIVATE DECENZA_BARISTA=1)
endif()
