/*
    Proteus -- High-performance query processing on heterogeneous hardware.

                            Copyright (c) 2025
        Data Intensive Applications and Systems Laboratory (DIAS)
                École Polytechnique Fédérale de Lausanne

                            All Rights Reserved.

    Permission to use, copy, modify and distribute this software and
    its documentation is hereby granted, provided that both the
    copyright notice and this permission notice appear in all copies of
    the software, derivative works or modified versions, and any
    portions thereof, and that both notices appear in supporting
    documentation.

    This code is distributed in the hope that it will be useful, but
    WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. THE AUTHORS
    DISCLAIM ANY LIABILITY OF ANY KIND FOR ANY DAMAGES WHATSOEVER
    RESULTING FROM THE USE OF THIS SOFTWARE.
*/

#include <cli-flags.hpp>
#include <iostream>
#include <olap/plan/prepared-statement.hpp>
#include <platform/topology/affinity_manager.hpp>
#include <platform/topology/topology.hpp>
#include <platform/util/glog.hpp>
#include <ssb/query.hpp>
#include <storage/storage-manager.hpp>
#include <query-shaping/experimental-shapers.hpp>

DECLARE_int32(ssb_scale_factor);
DEFINE_int32(
    ssb_scale_factor, 100,
    "Scale factor for the SSB benchmark. This determines the size of the dataset to be used. "
    "Default is 100. Data must be placed/linked in the inputs/ssbm<scale_factor> directory.");

// https://stackoverflow.com/a/25829178/1237824
std::string trim(const std::string &str) {
  size_t first = str.find_first_not_of(' ');
  if (std::string::npos == first) return str;
  size_t last = str.find_last_not_of(' ');
  return str.substr(first, (last - first + 1));
}

constexpr size_t clen(const char *str) { return (*str == 0) ? 0 : clen(str + 1) + 1; }

/**
 * Protocol:
 *
 * Communication is done over stdin/stdout
 * Command spans at most one line
 * Every line either starts with a command keyword or it should be IGNORED and
 *      considered a comment
 * Input commands:
 *
 *      quit
 *          Kills the engine
 *
 *      execute <query_name>
 *         Executes the query with the given name. Writes query duration to stdout
 *
 *
 *
 * Output commands:
 *      ready
 *          Send to the client when the raw-jit-executor is ready to start
 *          receiving commands
 *      error [(<reason>)]
 *          Specifies that a previous command or the engine failed.
 *          The optional (<reason>) specified in parenthesis a human-readable
 *          explanation of the error. The error may be fatal or not.
 */

int main(int argc, char *argv[]) {
  auto ctx = proteus::from_cli::olap("Simple command line interface for proteus", &argc, &argv);

  bool echo = false;

  set_exec_location_on_scope aff{topology::getInstance().getCpuNumaNodes()[0]};

  const size_t SF = 100;
  auto stats = ssb::Query::getStats(SF);
  LOG(INFO) << "preparing SSB at scale factor: " << SF;
  // store in a map so we can access them by name
  std::unordered_map<std::string, PreparedStatement> prepared_statements_map;
  {
    proteus::GPUOnlySingleServer shaper{"inputs/ssbm" + std::to_string(SF) + "/", stats,
                                        /*allow_moves=*/true};
    std::vector<PreparedStatement> prepared_statements = ssb::Query::prepareAll(shaper);
    // hard coded to SSB
    std::vector<std::string> query_names = {"q11", "q12", "q13", "q21", "q22", "q23", "q31",
                                            "q32", "q33", "q34", "q41", "q42", "q43"};
    CHECK_EQ(prepared_statements.size(), query_names.size());
    for (size_t i = 0; i < prepared_statements.size() && i < query_names.size(); ++i) {
      prepared_statements_map.emplace(query_names[i], std::move(prepared_statements[i]));
    }
  }

  LOG(INFO) << "Eagerly loading files to DRAM ...";
  // run 1 query from each query flight
  // by default this will load the required columns into DRAM (can be configured to load to GPU
  // memory as well)

  std::vector<std::string> warm_up_queries = {"q13", "q23", "q34", "q43"};
  for (const auto &query_name : warm_up_queries) {
    auto &stmt = prepared_statements_map.at(query_name);
    LOG(INFO) << "Running query: " << query_name;
    time_block t{[&](const auto &x) { LOG(INFO) << "Query " << query_name << " took: " << x; }};
    auto res = stmt.execute();
    LOG(INFO) << "Query result: " << res;
  }

  LOG(INFO) << "Finished initialization";
  std::cout << "ready" << std::endl;

  std::string line;
  while (std::getline(std::cin, line)) {
    std::string cmd = trim(line);
    LOG(INFO) << "Command received: " << cmd;

    if (cmd == "quit") {
      LOG(INFO) << "quiting..." << std::endl;
      auto &sm = StorageManager::getInstance();
      sm.unloadAll();
      break;
    } else if (cmd.starts_with("execute ")) {
      std::string query_name = cmd.substr(clen("execute "));
      query_name = trim(query_name);
      if (query_name.empty()) {
        std::cout << "error (no query specified)" << std::endl;
        continue;
      }

      auto it = prepared_statements_map.find(query_name);
      if (it == prepared_statements_map.end()) {
        std::cout << "error (unknown query: " << query_name << ")" << std::endl;
        continue;
      }

      auto &stmt = it->second;
      LOG(INFO) << "Running query: " << query_name;

      time_block t{[&](const auto &x) {
        LOG(INFO) << "Query" << query_name << " took: " << x;
        std::cout << x << std::endl;
      }};
      try {
        auto res = stmt.execute();
      } catch (const std::exception &e) {
        LOG(ERROR) << "Error executing query: " << e.what();
        std::cout << "error (" << e.what() << ")" << std::endl;
      }
    } else {
      // ignore comments
      LOG(INFO) << "Ignoring command: " << cmd;
    }
  }
  return 0;
}
