get_filename_component(_TEST_NAME ${CMAKE_CURRENT_LIST_DIR} NAME)

add_executable(${_TEST_NAME}_tests
	# --- TigerSlicer ------------------------------------------------
	test_adaptive_layers.cpp
	test_auto_outcome_feedback.cpp
	test_auto_plan_application.cpp
	test_auto_plan_optimizer.cpp
	test_batch_acceptance.cpp
	test_batch_preview.cpp
	test_batch_semantics.cpp
	test_bounded_batch_scheduler.cpp
	test_bridge_planner.cpp
	test_calibration_map.cpp
	test_calibration_prints.cpp
	test_connection_diagnostics.cpp
	test_cooling_scheduler.cpp
	test_dependency_components.cpp
	test_descent_proof.cpp
	test_detail_decomposition.cpp
	test_fdm_auto_plan.cpp
	test_final_file_verification.cpp
	test_flow_curves.cpp
	test_gcode_twin.cpp
	test_independent_tool_layer_schedule.cpp
	test_mixed_nozzle_support.cpp
	test_msla_auto_plan.cpp
	test_mslicer_resin_job.cpp
	test_orientation_advisor.cpp
	test_physical_printer_flow.cpp
	test_plan_explorer.cpp
	test_plate_compatibility.cpp
	test_pressure_planner.cpp
	test_preview_diagnostics.cpp
	test_print_history.cpp
	test_print_job_queue.cpp
	test_print_manifest.cpp
	test_printer_capability_match.cpp
	test_printer_capability_registry.cpp
	test_printer_maintenance.cpp
	test_printer_notifications.cpp
	test_printer_overview.cpp
	test_printer_quick_actions.cpp
	test_profile_versioning.cpp
	test_recovery_checkpoints.cpp
	test_resin_bridge_protocol.cpp
	test_resin_calibration.cpp
	test_resin_format_selection.cpp
	test_resin_issue_report.cpp
	test_resin_job_manifest.cpp
	test_resin_profile_migration.cpp
	test_resin_quality.cpp
	test_seam_optimizer.cpp
	test_slice_cache.cpp
	test_stage_a_acceptance.cpp
	test_stage_a_scenarios.cpp
	test_station_planner.cpp
	test_structural_infill.cpp
	test_support_interface.cpp
	test_swept_collision.cpp
	test_technology_capabilities.cpp
	test_telemetry_import.cpp
	test_thermal_planner.cpp
	test_tool_assignment.cpp
	test_tool_ownership.cpp
	test_tool_state_sync.cpp
	test_toolchange_cost.cpp
	test_toolchange_optimizer.cpp
	test_toolchanger_preflight.cpp
	test_transition_matrix.cpp
	test_upload_guard.cpp
    ${_TEST_NAME}_tests.cpp
    test_3mf.cpp
    test_aabbindirect.cpp
    test_appconfig.cpp
    test_arachne_walls.cpp
    test_arrange.cpp
    test_bambu_networking.cpp
    test_calib.cpp
    test_clipper_offset.cpp
    test_clipper_utils.cpp
    test_config.cpp
    test_config_variant_expansion.cpp
    test_toolordering_nozzle_group.cpp
    test_preset_bundle_loading.cpp
    test_preset_setting_id.cpp
    test_preset_diff.cpp
    test_elephant_foot_compensation.cpp
    test_fill_corner_smoothing.cpp
    test_fill_plane_path.cpp
    test_geometry.cpp
    test_multimaterial_segmentation.cpp
    test_placeholder_parser.cpp
    test_polygon.cpp
    test_mutable_polygon.cpp
    test_mutable_priority_queue.cpp
    test_nozzle_volume_type.cpp
    test_stl.cpp
    test_meshboolean.cpp
    test_marchingsquares.cpp
    test_model.cpp
    test_utils.cpp
    test_timeutils.cpp
    test_voronoi.cpp
    test_optimizers.cpp
    test_ordering_strategies.cpp
    # test_png_io.cpp
    test_indexed_triangle_set.cpp
    ../libnest2d/printer_parts.cpp
    )

if (TARGET OpenVDB::openvdb)
    target_sources(${_TEST_NAME}_tests PRIVATE test_hollowing.cpp)
endif()

target_link_libraries(${_TEST_NAME}_tests test_common libslic3r Catch2::Catch2WithMain)
target_include_directories(${_TEST_NAME}_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
set_property(TARGET ${_TEST_NAME}_tests PROPERTY FOLDER "tests")

orcaslicer_copy_test_dlls()

orcaslicer_discover_tests(${_TEST_NAME}_tests)
