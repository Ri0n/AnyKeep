cmake_minimum_required(VERSION 3.25)

foreach(
  required_var
  PACKAGE_PATH
  OUTPUT_PATH
  VERSION
  REQUIRED_VC_RUNTIME_VERSION
  UPGRADE_CODE
  BUNDLE_TEMPLATE
  LICENSE_TXT
  LICENSE_TEMPLATE
  LOGO_FILE
  WORK_ROOT)
  if(NOT DEFINED ${required_var} OR "${${required_var}}" STREQUAL "")
    message(FATAL_ERROR "${required_var} must be provided with -D")
  endif()
endforeach()

if(NOT WIN32)
  message(FATAL_ERROR "Burn packaging must run on Windows")
endif()
if(NOT VERSION MATCHES "^[0-9]+[.][0-9]+[.][0-9]+$")
  message(FATAL_ERROR "VERSION must be a numeric x.y.z version, got: ${VERSION}")
endif()
if(NOT REQUIRED_VC_RUNTIME_VERSION MATCHES "^[0-9]+[.][0-9]+[.][0-9]+[.][0-9]+$")
  message(
    FATAL_ERROR "REQUIRED_VC_RUNTIME_VERSION must have four numeric components, got: ${REQUIRED_VC_RUNTIME_VERSION}")
endif()

cmake_path(ABSOLUTE_PATH PACKAGE_PATH NORMALIZE)
cmake_path(ABSOLUTE_PATH OUTPUT_PATH NORMALIZE)
cmake_path(ABSOLUTE_PATH BUNDLE_TEMPLATE NORMALIZE)
cmake_path(ABSOLUTE_PATH LICENSE_TXT NORMALIZE)
cmake_path(ABSOLUTE_PATH LICENSE_TEMPLATE NORMALIZE)
cmake_path(ABSOLUTE_PATH LOGO_FILE NORMALIZE)
cmake_path(ABSOLUTE_PATH WORK_ROOT NORMALIZE)

foreach(required_file PACKAGE_PATH BUNDLE_TEMPLATE LICENSE_TXT LICENSE_TEMPLATE LOGO_FILE)
  if(NOT EXISTS "${${required_file}}")
    message(FATAL_ERROR "${required_file} does not exist: ${${required_file}}")
  endif()
endforeach()

find_program(wix_executable NAMES wix.exe wix REQUIRED)
find_program(curl_executable NAMES curl.exe curl REQUIRED)

if(NOT DEFINED VC_REDIST_PERMALINK OR VC_REDIST_PERMALINK STREQUAL "")
  set(VC_REDIST_PERMALINK "https://aka.ms/vc14/vc_redist.x64.exe")
endif()

file(REMOVE_RECURSE "${WORK_ROOT}")
file(MAKE_DIRECTORY "${WORK_ROOT}")
get_filename_component(output_dir "${OUTPUT_PATH}" DIRECTORY)
file(MAKE_DIRECTORY "${output_dir}")

set(payload_file "${WORK_ROOT}/vc_redist.x64.exe")
set(payload_source "${WORK_ROOT}/vc-redist-remote-payload.wxs")
set(bundle_source "${WORK_ROOT}/AnyKeepInstaller.wixbundle")
set(LICENSE_RTF "${WORK_ROOT}/GPLv3.rtf")

# The aka.ms endpoint is a rolling permalink. Capture the final URL used for these exact bytes so a Burn executable
# built today never combines tomorrow's payload with today's SHA-512. Restrict every hop to HTTPS.
execute_process(
  COMMAND "${curl_executable}" --location --fail --silent --show-error --retry 3 --retry-all-errors --proto "=https"
          --proto-redir "=https" --output "${payload_file}" --write-out "%{url_effective}" "${VC_REDIST_PERMALINK}"
  RESULT_VARIABLE download_result
  OUTPUT_VARIABLE resolved_url
  ERROR_VARIABLE download_error)
if(NOT download_result EQUAL 0)
  message(
    FATAL_ERROR "Failed to download the Visual C++ Redistributable from ${VC_REDIST_PERMALINK}: ${download_error}")
endif()
string(STRIP "${resolved_url}" resolved_url)
if(NOT resolved_url MATCHES "^https://download[.]visualstudio[.]microsoft[.]com/")
  message(FATAL_ERROR "The Visual C++ Redistributable permalink resolved to an unexpected host: ${resolved_url}. "
                      "Review Microsoft's redirect target before changing the allow-list.")
endif()
if(NOT EXISTS "${payload_file}")
  message(FATAL_ERROR "curl reported success but did not create ${payload_file}")
endif()

# Let WiX derive the authoritative remote-payload metadata (SHA-512, size, product metadata and file version) instead of
# duplicating its rules in CMake. -basepath keeps Name=vc_redist.x64.exe instead of embedding a build-machine path.
execute_process(
  COMMAND "${wix_executable}" burn remotepayload -basepath "${WORK_ROOT}" -downloadurl "${resolved_url}" -packagetype
          exe -out "${payload_source}" "${payload_file}"
  RESULT_VARIABLE payload_result
  OUTPUT_VARIABLE payload_stdout
  ERROR_VARIABLE payload_stderr)
if(NOT payload_result EQUAL 0)
  message(FATAL_ERROR "wix burn remotepayload failed for ${payload_file}: ${payload_stdout}\n${payload_stderr}")
endif()

file(READ "${payload_source}" payload_authoring)
string(REGEX MATCH "<ExePackagePayload[^>]*/>" ANYKEEP_VC_REDIST_PAYLOAD_XML "${payload_authoring}")
if(ANYKEEP_VC_REDIST_PAYLOAD_XML STREQUAL "")
  message(FATAL_ERROR "WiX did not generate an ExePackagePayload in ${payload_source}")
endif()
foreach(required_attribute Hash Size Version DownloadUrl)
  if(NOT ANYKEEP_VC_REDIST_PAYLOAD_XML MATCHES "${required_attribute}=\"")
    message(
      FATAL_ERROR "Generated Visual C++ payload is missing ${required_attribute}: ${ANYKEEP_VC_REDIST_PAYLOAD_XML}")
  endif()
endforeach()

function(escape_rtf input output)
  set(value "${input}")
  string(REPLACE "\\" "\\\\" value "${value}")
  string(REPLACE "{" "\\{" value "${value}")
  string(REPLACE "}" "\\}" value "${value}")
  string(REPLACE "\r\n" "\n" value "${value}")
  string(REPLACE "\r" "\n" value "${value}")
  string(REPLACE "\n" "\\par\n" value "${value}")
  set(${output}
      "${value}"
      PARENT_SCOPE)
endfunction()

file(READ "${LICENSE_TXT}" LICENSE_TEXT)
escape_rtf("${LICENSE_TEXT}" LICENSE_RTF_TEXT)
configure_file("${LICENSE_TEMPLATE}" "${LICENSE_RTF}" @ONLY)

set(VERSION_FOR_WIX "${VERSION}.0")
set(ANYKEEP_REQUIRED_VC_RUNTIME_VERSION "${REQUIRED_VC_RUNTIME_VERSION}")
set(ANYKEEP_MSI "${PACKAGE_PATH}")
configure_file("${BUNDLE_TEMPLATE}" "${bundle_source}" @ONLY)

execute_process(
  COMMAND "${wix_executable}" build "${bundle_source}" -arch x64 -ext WixToolset.BootstrapperApplications.wixext -ext
          WixToolset.Util.wixext -o "${OUTPUT_PATH}"
  # WixStdBA resolves LicenseFile by its bundle payload name (GPLv3.rtf). The previous in-tree Burn target happened to
  # run WiX from the directory containing that generated file; keep the standalone script equally deterministic.
  WORKING_DIRECTORY "${WORK_ROOT}"
  RESULT_VARIABLE build_result
  OUTPUT_VARIABLE build_stdout
  ERROR_VARIABLE build_stderr)
if(NOT build_result EQUAL 0)
  message(FATAL_ERROR "wix build failed for Burn bootstrapper: ${build_stdout}\n${build_stderr}")
endif()

message(STATUS "Visual C++ Redistributable permalink: ${VC_REDIST_PERMALINK}")
message(STATUS "Visual C++ Redistributable resolved URL: ${resolved_url}")
message(STATUS "Built Burn bootstrapper: ${OUTPUT_PATH}")
