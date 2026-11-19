# カバレッジ計測用ターゲット `coverage` を定義する。
#
# 使い方:
#   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=TestPlus
#   cmake --build build --target coverage
#
# 出力 (${CMAKE_BINARY_DIR}/coverage):
#   index.html     ... HTML レポート
#   coverage.info  ... lcov 形式 (VS Code Coverage Gutters 用)
#   coverage.xml   ... Cobertura XML (CI 用)
#
# 要件: gcovr (例: pipx install gcovr)。TestPlus 構成でのみ有効。

if(NOT CMAKE_BUILD_TYPE STREQUAL "TestPlus")
	message(STATUS "Coverage: disabled (CMAKE_BUILD_TYPE is not TestPlus)")
	return()
endif()

find_program(GCOVR_EXECUTABLE gcovr)
if(NOT GCOVR_EXECUTABLE)
	message(STATUS "Coverage: disabled (gcovr not found; try 'pipx install gcovr')")
	return()
endif()

set(COVERAGE_DIRECTORY ${CMAKE_BINARY_DIR}/coverage)

# tests 配下 (サブディレクトリ含む) の全ターゲットを依存に含める
function(collect_targets directory out)
	get_property(targets DIRECTORY ${directory} PROPERTY BUILDSYSTEM_TARGETS)
	get_property(subdirs DIRECTORY ${directory} PROPERTY SUBDIRECTORIES)
	foreach(subdir IN LISTS subdirs)
		collect_targets(${subdir} sub_targets)
		list(APPEND targets ${sub_targets})
	endforeach()
	set(${out} ${targets} PARENT_SCOPE)
endfunction()
collect_targets(${TEST_DIRECTORY} COVERAGE_TEST_TARGETS)

add_custom_target(coverage
	# 前回実行の .gcda を消して、今回のテスト分だけを計測する
	COMMAND ${CMAKE_COMMAND} -E make_directory ${COVERAGE_DIRECTORY}
	COMMAND find ${CMAKE_BINARY_DIR} -name "*.gcda" -delete
	# テストが失敗してもレポートは出す (失敗は警告として表示)
	COMMAND sh -c "'${CMAKE_CTEST_COMMAND}' --output-on-failure || echo 'WARNING: some tests failed'"
	COMMAND ${GCOVR_EXECUTABLE}
		--root ${HOME}
		--filter "${INCLUDE_DIRECTORY}/"
		--filter "${SOURCE_DIRECTORY}/"
		--exclude-throw-branches
		--exclude-unreachable-branches
		--print-summary
		--html-details ${COVERAGE_DIRECTORY}/index.html
		--lcov ${COVERAGE_DIRECTORY}/coverage.info
		--cobertura ${COVERAGE_DIRECTORY}/coverage.xml
		${CMAKE_BINARY_DIR}
	WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
	DEPENDS ${PROJECT_NAME} ${COVERAGE_TEST_TARGETS}
	COMMENT "Running tests and generating coverage report"
	VERBATIM
)
