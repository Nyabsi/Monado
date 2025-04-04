// Copyright 2024, Duncan Spaulding.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Lighthouse_console wrapper implementation
 * @author BabbleBones <BabbleBones@protonmail.com>
 * @ingroup drv_steamvr_lh
 */

#include "lh_console.hpp"
#include <csignal>
#include <cstddef>
#include <cstring>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <chrono>
#include <errno.h>

lighthouse_console::lighthouse_console(const std::string &path)
    : log_level(U_LOGGING_DEBUG), process_running(false), read_buffer(BUFFER_SIZE)
{
	if (pipe(stdin_pipe) < 0 || pipe(stdout_pipe) < 0) {
		LHC_ERR("Failed to create pipes: %s", strerror(errno));
		return;
	}

	child_pid = fork();
	if (child_pid < 0) {
		LHC_ERR("Fork failed: %s", strerror(errno));
		return;
	}

	if (child_pid == 0) { // Child process
		close(stdin_pipe[1]);
		close(stdout_pipe[0]);

		// Redirect standard file descriptors
		if (dup2(stdout_pipe[1], STDOUT_FILENO) < 0 || dup2(stdout_pipe[1], STDERR_FILENO) < 0 ||
		    dup2(stdin_pipe[0], STDIN_FILENO) < 0) {
			exit(1);
		}

		close(stdin_pipe[0]);
		close(stdout_pipe[1]);

		execl(path.c_str(), "lighthouse_console", (char *)NULL);
		exit(1);
	}

	// Parent process
	close(stdin_pipe[0]);
	close(stdout_pipe[1]);
	process_running = true;

	// Wait for initial prompt
	std::string initial_output;
	if (!wait_for_prompt(initial_output)) {
		LHC_ERR("Failed to get initial prompt");
		process_running = false;
	}
}

lighthouse_console::~lighthouse_console()
{
	if (process_running) {
		std::lock_guard<std::mutex> lock(console_mutex);

		// Force kill the process
		kill(child_pid, SIGTERM);

		// Wait for it to finish
		int status;
		waitpid(child_pid, &status, 0);

		// Close pipes
		close(stdin_pipe[1]);
		close(stdout_pipe[0]);
		process_running = false;
	}
}

bool
lighthouse_console::is_process_alive() const
{
	if (child_pid <= 0)
		return false;

	int status;
	pid_t result = waitpid(child_pid, &status, WNOHANG);
	if (result == 0)
		return true;
	if (result == child_pid)
		return false;
	return false;
}

bool
lighthouse_console::wait_for_prompt(std::string &output)
{
	auto start_time = std::chrono::steady_clock::now();

	while (true) {
		auto now = std::chrono::steady_clock::now();
		auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count();

		if (elapsed >= COMMAND_TIMEOUT_SEC) {
			LHC_ERR("Timeout waiting for prompt");
			return false;
		}

		fd_set readfds;
		struct timeval tv;
		tv.tv_sec = 1;
		tv.tv_usec = 0;

		FD_ZERO(&readfds);
		FD_SET(stdout_pipe[0], &readfds);

		int select_result = select(stdout_pipe[0] + 1, &readfds, NULL, NULL, &tv);
		if (select_result < 0) {
			if (errno == EINTR)
				continue;
			LHC_ERR("Select failed: %s", strerror(errno));
			return false;
		}

		if (select_result > 0) {
			ssize_t bytes_read = read(stdout_pipe[0], read_buffer.data(), read_buffer.size() - 1);
			if (bytes_read <= 0) {
				if (errno == EINTR)
					continue;
				LHC_ERR("Read failed: %s", strerror(errno));
				return false;
			}

			read_buffer[bytes_read] = '\0';
			output += read_buffer.data();

			// Check for prompt
			if (output.find(PROMPT) != std::string::npos) {
				return true;
			}
		}
	}
}

std::string
lighthouse_console::read_until_prompt()
{
	std::string output;
	if (!wait_for_prompt(output)) {
		LHC_ERR("Failed to read response");
		return "";
	}
	return output;
}

std::string
lighthouse_console::execute_command(const std::string &cmd)
{
	std::lock_guard<std::mutex> lock(console_mutex);

	if (!process_running || !is_process_alive()) {
		LHC_ERR("lighthouse_console process not running");
		return "";
	}

	LHC_INFO("Executing command: %s", cmd.c_str());

	std::string cmd_with_newline = cmd + "\n";
	ssize_t bytes_written = write(stdin_pipe[1], cmd_with_newline.c_str(), cmd_with_newline.length());
	if (bytes_written != static_cast<ssize_t>(cmd_with_newline.length())) {
		LHC_ERR("Failed to write command: %s", strerror(errno));
		return "";
	}

	std::string response = read_until_prompt();
	LHC_TRACE("Response: %s", response.c_str());
	return response;
}

void
lighthouse_console::select_dongle(const std::string &serial)
{
	if (!process_running) {
		LHC_ERR("lighthouse_console process not running");
		return;
	}

	LHC_DEBUG("Selecting dongle %s", serial.c_str());
	std::string response = execute_command("serial " + serial);
	LHC_TRACE("Select response: %s", response.c_str());
}

std::vector<lighthouse_console::dongle_info>
lighthouse_console::parse_dongle_list(const std::string &response)
{
	std::vector<dongle_info> dongles;
	std::string prefix = "Dongles:";

	size_t start = response.find(prefix);
	if (start == std::string::npos) {
		LHC_WARN("No dongle list found in response");
		return dongles;
	}

	std::string dongle_list = response.substr(start + prefix.length());
	std::stringstream ss(dongle_list);
	std::string dongle_entry;

	while (std::getline(ss, dongle_entry, ';')) {
		size_t first_comma = dongle_entry.find(',');
		if (first_comma != std::string::npos) {
			dongle_info info;
			info.serial = dongle_entry.substr(0, first_comma);

			size_t second_comma = dongle_entry.find(',', first_comma + 1);
			if (second_comma != std::string::npos) {
				std::string vrc_field = dongle_entry.substr(second_comma + 1);
				info.is_paired = vrc_field.find("VRC-") != std::string::npos;
			} else {
				info.is_paired = false;
			}

			dongles.push_back(info);
		}
	}

	return dongles;
}

std::vector<std::string>
lighthouse_console::list_connected_dongles()
{
	std::vector<std::string> serials = {}; // Zero initialize in case of trouble

	if (!process_running) {
		LHC_ERR("lighthouse_console process not running");
		return serials;
	}

	LHC_DEBUG("Listing connected dongles");
	std::string response = execute_command("dongleinfo");
	auto dongles = parse_dongle_list(response);

	for (const auto &dongle : dongles) {
		serials.push_back(dongle.serial);
	}
	return serials;
}

/* PLEASE NOTE THIS FUNCTION IS UNRELIABLE FOR PROVING PAIR, RF interference
 * can block the signal and device crosstalk as well making it impossible to
 * know who is all connected at any given time, more reliable to take stock
 * of all dongles and then wait for each one to find a device.
 */

std::vector<std::string>
lighthouse_console::list_paired_dongles()
{
	std::vector<std::string> paired_serials = {};

	if (!process_running) {
		LHC_ERR("lighthouse_console process not running");
		return paired_serials;
	}

	LHC_DEBUG("Listing paired dongles");
	std::string response = execute_command("dongleinfo");
	auto dongles = parse_dongle_list(response);

	for (const auto &dongle : dongles) {
		if (dongle.is_paired) {
			LHC_DEBUG("Found paired dongle: %s", dongle.serial.c_str());
			paired_serials.push_back(dongle.serial);
		}
	}

	LHC_DEBUG("Found %zu paired dongles", paired_serials.size());
	return paired_serials;
}

bool
lighthouse_console::is_dongle_paired(const std::string &serial)
{
	if (!process_running) {
		LHC_ERR("lighthouse_console process not running");
		return false;
	}

	LHC_DEBUG("Checking if dongle %s is paired", serial.c_str());
	std::string response = execute_command("dongleinfo");
	auto dongles = parse_dongle_list(response);

	for (const auto &dongle : dongles) {
		if (dongle.serial == serial) {
			LHC_DEBUG("Dongle %s paired status: %s", serial.c_str(), dongle.is_paired ? "true" : "false");
			return dongle.is_paired;
		}
	}

	LHC_DEBUG("Dongle %s not found", serial.c_str());
	return false;
}

void
lighthouse_console::pair_dongle(const std::string &serial)
{
	if (!process_running) {
		LHC_ERR("lighthouse_console process not running");
		return;
	}

	LHC_INFO("Attempting to pair dongle %s", serial.c_str());
	select_dongle(serial);

	std::string response = execute_command("pair");
	LHC_TRACE("Pair response: %s", response.c_str());
}

void
lighthouse_console::unpair_dongle(const std::string &serial)
{
	if (!process_running) {
		LHC_ERR("lighthouse_console process not running");
		return;
	}

	LHC_INFO("Attempting to unpair dongle %s", serial.c_str());
	select_dongle(serial);

	std::string response = execute_command("unpair");
	LHC_TRACE("Unpair response: %s", response.c_str());
}

void
lighthouse_console::power_off_dongle(const std::string &serial)
{
	if (!process_running) {
		LHC_ERR("lighthouse_console process not running");
		return;
	}

	LHC_INFO("Powering off dongle %s", serial.c_str());
	select_dongle(serial);

	std::string response = execute_command("poweroff");
	LHC_TRACE("Power off response: %s", response.c_str());
}

void
lighthouse_console::identify_dongle(const std::string &serial)
{
	if (!process_running) {
		LHC_ERR("lighthouse_console process not running");
		return;
	}

	LHC_INFO("Promping identify from dongle %s", serial.c_str());
	select_dongle(serial);

	std::string response = execute_command("identifycontroller");
	LHC_TRACE("Identify response: %s", response.c_str());
}

void
lighthouse_console::pair_all_dongles()
{
	if (!process_running) {
		LHC_ERR("lighthouse_console process not running");
		return;
	}

	LHC_INFO("Pairing all dongles");
	std::string response = execute_command("pairall");
	LHC_TRACE("Pair all response: %s", response.c_str());
}

void
lighthouse_console::unpair_all_dongles()
{
	if (!process_running) {
		LHC_ERR("lighthouse_console process not running");
		return;
	}

	LHC_INFO("Unpairing all dongles");
	std::string response = execute_command("unpairall");
	LHC_TRACE("Unpair all response: %s", response.c_str());
}

void
lighthouse_console::force_pair_all_dongles()
{
	if (!process_running) {
		LHC_ERR("lighthouse_console process not running");
		return;
	}

	LHC_INFO("Force pairing all dongles");
	std::string response = execute_command("forcepairall");
	LHC_TRACE("Force pair all response: %s", response.c_str());
}