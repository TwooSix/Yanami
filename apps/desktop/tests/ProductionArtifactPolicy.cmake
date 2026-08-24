if(NOT DEFINED YANAMI_SOURCE_DIR OR NOT IS_DIRECTORY "${YANAMI_SOURCE_DIR}")
    message(FATAL_ERROR "YANAMI_SOURCE_DIR is missing: ${YANAMI_SOURCE_DIR}")
endif()
if(NOT DEFINED YANAMI_EXECUTABLE OR NOT EXISTS "${YANAMI_EXECUTABLE}")
    message(FATAL_ERROR "YANAMI_EXECUTABLE is missing: ${YANAMI_EXECUTABLE}")
endif()
if(NOT DEFINED YANAMI_BRIDGE OR NOT EXISTS "${YANAMI_BRIDGE}")
    message(FATAL_ERROR "YANAMI_BRIDGE is missing: ${YANAMI_BRIDGE}")
endif()

file(GLOB_RECURSE native_sources LIST_DIRECTORIES FALSE
    "${YANAMI_SOURCE_DIR}/*.cpp"
    "${YANAMI_SOURCE_DIR}/*.hpp")
foreach(source_file IN LISTS native_sources)
    if(source_file MATCHES "[/\\\\]DevelopmentHooks\\.cpp$")
        continue()
    endif()
    file(READ "${source_file}" source_contents)
    string(REGEX MATCH
        "YANAMI_DEV_[A-Z0-9_]+"
        leaked_source_hook
        "${source_contents}")
    if(leaked_source_hook)
        message(FATAL_ERROR
            "Development environment parsing escaped DevelopmentHooks.cpp: "
            "${source_file} (${leaked_source_hook})")
    endif()
endforeach()

set(mpv_source "${YANAMI_SOURCE_DIR}/MpvVideoItem.cpp")
file(READ "${mpv_source}" mpv_source_contents)
string(FIND "${mpv_source_contents}" "{\"tls-verify\", \"yes\"}" tls_option_position)
string(FIND "${mpv_source_contents}" "options/tls-verify" tls_verification_position)
if(tls_option_position EQUAL -1 OR tls_verification_position EQUAL -1)
    message(FATAL_ERROR
        "Production player must enable and verify libmpv TLS certificate validation")
endif()

file(STRINGS "${YANAMI_EXECUTABLE}" leaked_binary_hooks
    REGEX "YANAMI_DEV_[A-Z0-9_]+")
if(leaked_binary_hooks)
    list(REMOVE_DUPLICATES leaked_binary_hooks)
    list(JOIN leaked_binary_hooks ", " leaked_binary_text)
    message(FATAL_ERROR
        "Production executable contains development hook names: ${leaked_binary_text}")
endif()

set(required_bridge_symbols
    yanami_backend_abi_version
    yanami_backend_new
    yanami_backend_free
    yanami_backend_cancel_all
    yanami_string_free
    yanami_backend_dandanplay_credential_source
    yanami_backend_emby_connected
    yanami_backend_configure_dandanplay
    yanami_backend_clear_dandanplay
    yanami_backend_login_emby
    yanami_backend_logout_emby
    yanami_backend_emby_settings_json
    yanami_backend_refresh_progress_json
    yanami_backend_library_json
    yanami_backend_catalog_search_json
    yanami_backend_catalog_search_hydrate_images
    yanami_backend_activity_json
    yanami_backend_favorites_json
    yanami_backend_collection_json
    yanami_backend_metadata_json
    yanami_backend_update_metadata_json
    yanami_backend_playlist_targets_json
    yanami_backend_add_to_playlist_json
    yanami_backend_remove_from_playlist_json
    yanami_backend_image_editor_json
    yanami_backend_image_providers_json
    yanami_backend_image_search_json
    yanami_backend_image_apply_json
    yanami_backend_image_upload_json
    yanami_backend_image_delete_json
    yanami_backend_refresh_metadata_json
    yanami_backend_set_played_json
    yanami_backend_set_favorite_json
    yanami_backend_scan_library_files_json
    yanami_backend_delete_item_json
    yanami_backend_danmaku_search_json
    yanami_backend_danmaku_auto_json
    yanami_backend_danmaku_apply_json
    yanami_backend_playback_request_json
    yanami_backend_report_playback_json
)
file(STRINGS "${YANAMI_BRIDGE}" bridge_symbol_strings
    REGEX "yanami_(backend|string)_[a-z0-9_]+")
foreach(required_symbol IN LISTS required_bridge_symbols)
    string(FIND "${bridge_symbol_strings}" "${required_symbol}" symbol_position)
    if(symbol_position EQUAL -1)
        message(FATAL_ERROR
            "Rust bridge is missing required ABI v3 symbol: ${required_symbol}")
    endif()
endforeach()
set(forbidden_bridge_symbols
    yanami_backend_item_action_json
    yanami_backend_item_state_json
    yanami_backend_playback_json
)
foreach(forbidden_symbol IN LISTS forbidden_bridge_symbols)
    list(FIND required_bridge_symbols "${forbidden_symbol}" required_position)
    string(FIND "${bridge_symbol_strings}" "${forbidden_symbol}" symbol_position)
    if(required_position EQUAL -1 AND NOT symbol_position EQUAL -1)
        message(FATAL_ERROR
            "Rust bridge contains removed compatibility symbol: ${forbidden_symbol}")
    endif()
endforeach()
