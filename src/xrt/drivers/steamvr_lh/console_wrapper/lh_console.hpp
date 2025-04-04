// Copyright 2024, Duncan Spaulding.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Lighthouse_console wrapper interface
 * @author BabbleBones <BabbleBones@protonmail.com>
 * @ingroup drv_steamvr_lh
 */

#pragma once
#include "util/u_logging.h"
#include <string>
#include <vector>
#include <mutex>

// Logging macros for lighthouse console
#define LHC_ERR(...) U_LOG_IFL_E(log_level, __VA_ARGS__)
#define LHC_WARN(...) U_LOG_IFL_W(log_level, __VA_ARGS__)
#define LHC_INFO(...) U_LOG_IFL_I(log_level, __VA_ARGS__)
#define LHC_TRACE(...) U_LOG_IFL_T(log_level, __VA_ARGS__)
#define LHC_DEBUG(...) U_LOG_IFL_D(log_level, __VA_ARGS__)

class lighthouse_console
{
public:
	explicit lighthouse_console(const std::string &path);
	~lighthouse_console();

	lighthouse_console(const lighthouse_console &) = delete;
	lighthouse_console &
	operator=(const lighthouse_console &) = delete;

	// Device queries
	std::vector<std::string>
	list_connected_dongles();
	std::vector<std::string>
	list_paired_dongles();
	bool
	is_dongle_paired(const std::string &serial);

	// Single device operations
	void
	pair_dongle(const std::string &serial);
	void
	unpair_dongle(const std::string &serial);
	void
	power_off_dongle(const std::string &serial);
	void
	identify_dongle(const std::string &serial);

	// Bulk operations
	void
	pair_all_dongles();
	void
	unpair_all_dongles();
	void
	force_pair_all_dongles();

private:
	static constexpr size_t BUFFER_SIZE = 4096;
	static constexpr const char *PROMPT = "lh>";
	static constexpr int COMMAND_TIMEOUT_SEC = 5;

	enum u_logging_level log_level;
	struct dongle_info
	{
		std::string serial;
		bool is_paired;
	};

	// Process management
	pid_t child_pid;
	int stdin_pipe[2];
	int stdout_pipe[2];
	bool process_running;

	// Thread safety
	std::mutex console_mutex;

	// Buffer management
	std::vector<char> read_buffer;

	// Subprocess communication
	std::string
	execute_command(const std::string &cmd);
	void
	select_dongle(const std::string &serial);
	bool
	wait_for_prompt(std::string &output);
	std::string
	read_until_prompt();
	bool
	is_process_alive() const;

	std::vector<dongle_info>
	parse_dongle_list(const std::string &response);
};