// Copyright 2019 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "google/cloud/spanner/benchmarks/benchmarks_config.h"
#include "google/cloud/spanner/client.h"
#include "google/cloud/spanner/database_admin_client.h"
#include "google/cloud/spanner/internal/build_info.h"
#include "google/cloud/spanner/testing/pick_random_instance.h"
#include "google/cloud/spanner/testing/random_database_name.h"
#include "google/cloud/internal/getenv.h"
#include "google/cloud/internal/random.h"
#include <algorithm>
#include <future>
#include <random>
#include <sstream>
#include <thread>

namespace {

namespace cloud_spanner = google::cloud::spanner;
using ::google::cloud::spanner_benchmarks::Config;

struct SingleRowThroughputSample {
  int client_count;
  int thread_count;
  int event_count;
  std::chrono::microseconds elapsed;
};

using SampleSink = std::function<void(std::vector<SingleRowThroughputSample>)>;

class Experiment {
 public:
  virtual ~Experiment() = default;

  virtual void SetUp(Config const& config,
                     cloud_spanner::Database const& database) = 0;
  virtual void Run(Config const& config,
                   cloud_spanner::Database const& database,
                   SampleSink const& sink) = 0;
};

std::map<std::string, std::shared_ptr<Experiment>> AvailableExperiments();

}  // namespace

int main(int argc, char* argv[]) {
  Config config;
  {
    std::vector<std::string> args{argv, argv + argc};
    auto c = google::cloud::spanner_benchmarks::ParseArgs(args);
    if (!c) {
      std::cerr << "Error parsing command-line arguments: " << c.status()
                << "\n";
      return 1;
    }
    config = *std::move(c);
  }

  auto generator = google::cloud::internal::MakeDefaultPRNG();
  if (config.instance_id.empty()) {
    auto instance = google::cloud::spanner_testing::PickRandomInstance(
        generator, config.project_id);
    if (!instance) {
      std::cerr << "Error selecting an instance to run the experiment: "
                << instance.status() << "\n";
      return 1;
    }
    config.instance_id = *std::move(instance);
  }

  cloud_spanner::Database database(
      config.project_id, config.instance_id,
      google::cloud::spanner_testing::RandomDatabaseName(generator));
  config.database_id = database.database_id();

  auto available = AvailableExperiments();
  auto e = available.find(config.experiment);
  if (e == available.end()) {
    std::cerr << "Experiment " << config.experiment << " not found\n";
    return 1;
  }

  cloud_spanner::DatabaseAdminClient admin_client;
  auto created =
      admin_client.CreateDatabase(database, {R"sql(CREATE TABLE KeyValue (
                                Key   INT64 NOT NULL,
                                Data  STRING(1024),
                             ) PRIMARY KEY (Key))sql"});
  std::cout << "# Waiting for database creation to complete " << std::flush;
  for (;;) {
    auto status = created.wait_for(std::chrono::seconds(1));
    if (status == std::future_status::ready) break;
    std::cout << '.' << std::flush;
  }
  std::cout << " DONE\n";
  auto db = created.get();
  if (!db) {
    std::cerr << "Error creating database: " << db.status() << "\n";
    return 1;
  }

  std::cout << "ClientCount,ThreadCount,EventCount,ElapsedTime\n" << std::flush;

  std::mutex cout_mu;
  auto cout_sink =
      [&cout_mu](
          std::vector<SingleRowThroughputSample> const& samples) mutable {
        std::unique_lock<std::mutex> lk(cout_mu);
        for (auto const& s : samples) {
          std::cout << std::boolalpha << s.client_count << ',' << s.thread_count
                    << ',' << s.event_count << ',' << s.elapsed.count() << '\n'
                    << std::flush;
        }
      };

  auto experiment = e->second;
  experiment->SetUp(config, database);
  experiment->Run(config, database, cout_sink);

  auto drop = admin_client.DropDatabase(database);
  if (!drop.ok()) {
    std::cerr << "# Error dropping database: " << drop << "\n";
  }
  std::cout << "# Experiment finished, database dropped\n";
  return 0;
}

namespace {

using RandomKeyGenerator = std::function<std::int64_t()>;
using ErrorSink = std::function<void(std::vector<google::cloud::Status>)>;

void FillTableTask(Config const& config, cloud_spanner::Client client,
                   std::mutex& mu, std::string const& value, int task_count,
                   int task_id) {
  auto mutation =
      cloud_spanner::InsertOrUpdateMutationBuilder("KeyValue", {"Key", "Data"});
  int current_mutations = 0;

  auto maybe_flush = [&mutation, &current_mutations, &client, &mu](bool force) {
    if (current_mutations == 0) {
      return;
    }
    if (!force && current_mutations < 1000) {
      return;
    }
    auto m = std::move(mutation).Build();
    auto result = client.Commit([&m](cloud_spanner::Transaction const&) {
      return cloud_spanner::Mutations{m};
    });
    if (!result) {
      std::lock_guard<std::mutex> lk(mu);
      std::cerr << "# Error in Commit() " << result.status() << "\n";
    }
    mutation = cloud_spanner::InsertOrUpdateMutationBuilder("KeyValue",
                                                            {"Key", "Data"});
    current_mutations = 0;
  };
  auto force_flush = [&maybe_flush] { maybe_flush(true); };
  auto flush_as_needed = [&maybe_flush] { maybe_flush(false); };

  auto const report_period =
      (std::max)(static_cast<std::int64_t>(2), config.table_size / 50);
  for (std::int64_t key = 0; key != config.table_size; ++key) {
    // Each thread does a fraction of the key space.
    if (key % task_count != task_id) continue;
    // Have one of the threads report progress about 50 times.
    if (task_id == 0 && key % report_period == 0) {
      std::cout << '.' << std::flush;
    }
    mutation.EmplaceRow(key, value);
    current_mutations++;
    flush_as_needed();
  }
  force_flush();
}

void FillTable(Config const& config, cloud_spanner::Database const& database,
               std::mutex& mu, std::string const& value) {
  // We need to populate some data or all the requests to read will fail.
  cloud_spanner::Client client(cloud_spanner::MakeConnection(database));
  std::cout << "# Populating database " << std::flush;
  int const task_count = 16;
  std::vector<std::future<void>> tasks(task_count);
  int task_id = 0;
  for (auto& t : tasks) {
    t = std::async(
        std::launch::async,
        [&config, &client, &mu, &value](int tc, int ti) {
          FillTableTask(config, client, mu, value, tc, ti);
        },
        task_count, task_id++);
  }
  for (auto& t : tasks) {
    t.get();
  }
  std::cout << " DONE\n";
}

int ClientCount(Config const& config,
                google::cloud::internal::DefaultPRNG& generator,
                int thread_count) {
  // TODO(#1000) - avoid deadlocks with more than 100 threads per client
  auto min_clients = (std::max)(thread_count / 100 + 1, config.minimum_clients);
  auto const max_clients = config.maximum_clients;
  if (min_clients <= max_clients) {
    return min_clients;
  }
  return std::uniform_int_distribution<int>(min_clients,
                                            max_clients - 1)(generator);
};

class InsertOrUpdateExperiment : public Experiment {
 public:
  void SetUp(Config const&, cloud_spanner::Database const&) override {}

  void Run(Config const& config, cloud_spanner::Database const& database,
           SampleSink const& sink) override {
    std::cout << config << std::flush;
    // Create enough clients for the worst case
    std::vector<cloud_spanner::Client> clients;
    std::cout << "# Creating clients " << std::flush;
    for (int i = 0; i != config.maximum_clients; ++i) {
      clients.emplace_back(cloud_spanner::Client(cloud_spanner::MakeConnection(
          database, cloud_spanner::ConnectionOptions().set_channel_pool_domain(
                        "task:" + std::to_string(i)))));
      std::cout << '.' << std::flush;
    }
    std::cout << " DONE\n";

    auto generator = google::cloud::internal::MakeDefaultPRNG();
    std::uniform_int_distribution<int> thread_count_gen(config.minimum_threads,
                                                        config.maximum_threads);

    for (int i = 0; i != config.samples; ++i) {
      auto const thread_count = thread_count_gen(generator);
      auto const client_count = ClientCount(config, generator, thread_count);
      std::vector<cloud_spanner::Client> iteration_clients(
          clients.begin(), clients.begin() + client_count);
      RunIteration(config, iteration_clients, thread_count, sink, generator);
    }
  }

  void RunIteration(Config const& config,
                    std::vector<cloud_spanner::Client> const& clients,
                    int thread_count, SampleSink const& sink,
                    google::cloud::internal::DefaultPRNG generator) {
    std::mutex mu;
    std::uniform_int_distribution<std::int64_t> random_key(0,
                                                           config.table_size);
    RandomKeyGenerator locked_random_key = [&mu, &generator, &random_key] {
      std::lock_guard<std::mutex> lk(mu);
      return random_key(generator);
    };

    std::mutex cerr_mu;
    ErrorSink error_sink =
        [&cerr_mu](std::vector<google::cloud::Status> const& errors) {
          std::lock_guard<std::mutex> lk(cerr_mu);
          for (auto const& e : errors) {
            std::cerr << "# " << e << "\n";
          }
        };

    std::vector<std::future<int>> tasks(thread_count);
    auto start = std::chrono::steady_clock::now();
    int task_id = 0;
    for (auto& t : tasks) {
      auto client = clients[task_id++ % clients.size()];
      t = std::async(std::launch::async, &InsertOrUpdateExperiment::RunTask,
                     this, config, client, locked_random_key, error_sink);
    }
    int insert_count = 0;
    for (auto& t : tasks) {
      insert_count += t.get();
    }
    auto elapsed = std::chrono::steady_clock::now() - start;

    sink({SingleRowThroughputSample{
        static_cast<int>(clients.size()), thread_count, insert_count,
        std::chrono::duration_cast<std::chrono::microseconds>(elapsed)}});
  }

  int RunTask(Config const& config, cloud_spanner::Client client,
              RandomKeyGenerator const& key_generator,
              ErrorSink const& error_sink) {
    int count = 0;
    std::string value(1024, 'A');
    std::vector<google::cloud::Status> errors;
    for (auto start = std::chrono::steady_clock::now(),
              deadline = start + config.iteration_duration;
         start < deadline; start = std::chrono::steady_clock::now()) {
      auto key = key_generator();
      auto m = cloud_spanner::MakeInsertOrUpdateMutation(
          "KeyValue", {"Key", "Data"}, key, value);
      auto result = client.Commit([&m](cloud_spanner::Transaction const&) {
        return cloud_spanner::Mutations{m};
      });
      if (!result) {
        errors.push_back(std::move(result).status());
      }
      ++count;
    }
    error_sink(std::move(errors));
    return count;
  }
};

class ReadExperiment : public Experiment {
 public:
  ReadExperiment() : generator_(std::random_device{}()) {}

  void SetUp(Config const& config,
             cloud_spanner::Database const& database) override {
    std::string value = [this] {
      std::lock_guard<std::mutex> lk(mu_);
      return google::cloud::internal::Sample(
          generator_, 1024, "#@$%^&*()-=+_0123456789[]{}|;:,./<>?");
    }();
    FillTable(config, database, mu_, value);
  }

  void Run(Config const& config, cloud_spanner::Database const& database,
           SampleSink const& sink) override {
    std::cout << config << std::flush;
    // Create enough clients for the worst case
    std::vector<cloud_spanner::Client> clients;
    std::cout << "# Creating clients " << std::flush;
    for (int i = 0; i != config.maximum_clients; ++i) {
      clients.emplace_back(cloud_spanner::Client(cloud_spanner::MakeConnection(
          database, cloud_spanner::ConnectionOptions().set_channel_pool_domain(
                        "task:" + std::to_string(i)))));
      std::cout << '.' << std::flush;
    }
    std::cout << " DONE\n";

    std::uniform_int_distribution<int> thread_count_gen(config.minimum_threads,
                                                        config.maximum_threads);

    for (int i = 0; i != config.samples; ++i) {
      auto const thread_count = thread_count_gen(generator_);
      auto const client_count = ClientCount(config, generator_, thread_count);
      std::vector<cloud_spanner::Client> iteration_clients(
          clients.begin(), clients.begin() + client_count);
      RunIteration(config, iteration_clients, thread_count, sink);
    }
  }

  void RunIteration(Config const& config,
                    std::vector<cloud_spanner::Client> const& clients,
                    int thread_count, SampleSink const& sink) {
    std::uniform_int_distribution<std::int64_t> random_key(0,
                                                           config.table_size);
    RandomKeyGenerator locked_random_key = [this, &random_key] {
      std::lock_guard<std::mutex> lk(mu_);
      return random_key(generator_);
    };

    std::mutex cerr_mu;
    ErrorSink error_sink =
        [&cerr_mu](std::vector<google::cloud::Status> const& errors) {
          std::lock_guard<std::mutex> lk(cerr_mu);
          for (auto const& e : errors) {
            std::cerr << "# " << e << "\n";
          }
        };

    std::vector<std::future<int>> tasks(thread_count);
    auto start = std::chrono::steady_clock::now();
    int task_id = 0;
    for (auto& t : tasks) {
      auto client = clients[task_id++ % clients.size()];
      t = std::async(std::launch::async, &ReadExperiment::RunTask, this, config,
                     client, locked_random_key, error_sink);
    }
    int total_count = 0;
    for (auto& t : tasks) {
      total_count += t.get();
    }
    auto elapsed = std::chrono::steady_clock::now() - start;

    sink({SingleRowThroughputSample{
        static_cast<int>(clients.size()), thread_count, total_count,
        std::chrono::duration_cast<std::chrono::microseconds>(elapsed)}});
  }

  int RunTask(Config const& config, cloud_spanner::Client client,
              RandomKeyGenerator const& key_generator,
              ErrorSink const& error_sink) {
    int count = 0;
    std::string value(1024, 'A');
    std::vector<google::cloud::Status> errors;
    for (auto start = std::chrono::steady_clock::now(),
              deadline = start + config.iteration_duration;
         start < deadline; start = std::chrono::steady_clock::now()) {
      auto key = key_generator();
      auto rows = client.Read(
          "KeyValue",
          cloud_spanner::KeySet().AddKey(cloud_spanner::MakeKey(key)),
          {"Key", "Data"});
      for (auto& row :
           cloud_spanner::StreamOf<std::tuple<std::int64_t, std::string>>(
               rows)) {
        if (!row) {
          errors.push_back(std::move(row).status());
          break;
        }
        ++count;
      }
    }
    error_sink(std::move(errors));
    return count;
  }

 private:
  std::mutex mu_;
  google::cloud::internal::DefaultPRNG generator_;
};

class UpdateDmlExperiment : public Experiment {
 public:
  UpdateDmlExperiment() : generator_(std::random_device{}()) {}

  void SetUp(Config const& config,
             cloud_spanner::Database const& database) override {
    std::string value = [this] {
      std::lock_guard<std::mutex> lk(mu_);
      return google::cloud::internal::Sample(
          generator_, 1024, "#@$%^&*()-=+_0123456789[]{}|;:,./<>?");
    }();
    FillTable(config, database, mu_, value);
  }

  void Run(Config const& config, cloud_spanner::Database const& database,
           SampleSink const& sink) override {
    std::cout << config << std::flush;
    // Create enough clients for the worst case
    std::vector<cloud_spanner::Client> clients;
    std::cout << "# Creating clients " << std::flush;
    for (int i = 0; i != config.maximum_clients; ++i) {
      clients.emplace_back(cloud_spanner::Client(cloud_spanner::MakeConnection(
          database, cloud_spanner::ConnectionOptions().set_channel_pool_domain(
                        "task:" + std::to_string(i)))));
      std::cout << '.' << std::flush;
    }
    std::cout << " DONE\n";

    auto generator = google::cloud::internal::MakeDefaultPRNG();
    std::uniform_int_distribution<int> thread_count_gen(config.minimum_threads,
                                                        config.maximum_threads);

    for (int i = 0; i != config.samples; ++i) {
      auto const thread_count = thread_count_gen(generator);
      auto const client_count = ClientCount(config, generator, thread_count);
      std::vector<cloud_spanner::Client> iteration_clients(
          clients.begin(), clients.begin() + client_count);
      RunIteration(config, iteration_clients, thread_count, sink, generator);
    }
  }

  void RunIteration(Config const& config,
                    std::vector<cloud_spanner::Client> const& clients,
                    int thread_count, SampleSink const& sink,
                    google::cloud::internal::DefaultPRNG generator) {
    std::mutex mu;
    std::uniform_int_distribution<std::int64_t> random_key(0,
                                                           config.table_size);
    RandomKeyGenerator locked_random_key = [&mu, &generator, &random_key] {
      std::lock_guard<std::mutex> lk(mu);
      return random_key(generator);
    };

    std::mutex cerr_mu;
    ErrorSink error_sink =
        [&cerr_mu](std::vector<google::cloud::Status> const& errors) {
          std::lock_guard<std::mutex> lk(cerr_mu);
          for (auto const& e : errors) {
            std::cerr << "# " << e << "\n";
          }
        };

    std::vector<std::future<int>> tasks(thread_count);
    auto start = std::chrono::steady_clock::now();
    int task_id = 0;
    for (auto& t : tasks) {
      auto client = clients[task_id++ % clients.size()];
      t = std::async(std::launch::async, &UpdateDmlExperiment::RunTask, this,
                     config, client, locked_random_key, error_sink);
    }
    int insert_count = 0;
    for (auto& t : tasks) {
      insert_count += t.get();
    }
    auto elapsed = std::chrono::steady_clock::now() - start;

    sink({SingleRowThroughputSample{
        static_cast<int>(clients.size()), thread_count, insert_count,
        std::chrono::duration_cast<std::chrono::microseconds>(elapsed)}});
  }

  int RunTask(Config const& config, cloud_spanner::Client client,
              RandomKeyGenerator const& key_generator,
              ErrorSink const& error_sink) {
    int count = 0;
    std::string value(1024, 'A');
    std::vector<google::cloud::Status> errors;
    for (auto start = std::chrono::steady_clock::now(),
              deadline = start + config.iteration_duration;
         start < deadline; start = std::chrono::steady_clock::now()) {
      auto key = key_generator();
      auto result = client.Commit(
          [&client, key, &value](cloud_spanner::Transaction const& txn)
              -> google::cloud::StatusOr<cloud_spanner::Mutations> {
            auto result = client.ExecuteDml(
                txn, cloud_spanner::SqlStatement(
                         "UPDATE KeyValue SET Data = @data WHERE Key = @key",
                         {{"key", cloud_spanner::Value(key)},
                          {"data", cloud_spanner::Value(value)}}));
            if (!result) return std::move(result).status();
            return cloud_spanner::Mutations{};
          });
      if (!result) {
        errors.push_back(std::move(result).status());
      }
      ++count;
    }
    error_sink(std::move(errors));
    return count;
  }

 private:
  std::mutex mu_;
  google::cloud::internal::DefaultPRNG generator_;
};

class SelectExperiment : public Experiment {
 public:
  SelectExperiment() : generator_(std::random_device{}()) {}

  void SetUp(Config const& config,
             cloud_spanner::Database const& database) override {
    std::string value = [this] {
      std::lock_guard<std::mutex> lk(mu_);
      return google::cloud::internal::Sample(
          generator_, 1024, "#@$%^&*()-=+_0123456789[]{}|;:,./<>?");
    }();
    FillTable(config, database, mu_, value);
  }

  void Run(Config const& config, cloud_spanner::Database const& database,
           SampleSink const& sink) override {
    std::cout << config << std::flush;
    // Create enough clients for the worst case
    std::vector<cloud_spanner::Client> clients;
    std::cout << "# Creating clients " << std::flush;
    for (int i = 0; i != config.maximum_clients; ++i) {
      clients.emplace_back(cloud_spanner::Client(cloud_spanner::MakeConnection(
          database, cloud_spanner::ConnectionOptions().set_channel_pool_domain(
                        "task:" + std::to_string(i)))));
      std::cout << '.' << std::flush;
    }
    std::cout << " DONE\n";

    std::uniform_int_distribution<int> thread_count_gen(config.minimum_threads,
                                                        config.maximum_threads);

    for (int i = 0; i != config.samples; ++i) {
      auto const thread_count = thread_count_gen(generator_);
      auto const client_count = ClientCount(config, generator_, thread_count);
      std::vector<cloud_spanner::Client> iteration_clients(
          clients.begin(), clients.begin() + client_count);
      RunIteration(config, iteration_clients, thread_count, sink);
    }
  }

  void RunIteration(Config const& config,
                    std::vector<cloud_spanner::Client> const& clients,
                    int thread_count, SampleSink const& sink) {
    std::uniform_int_distribution<std::int64_t> random_key(0,
                                                           config.table_size);
    RandomKeyGenerator locked_random_key = [this, &random_key] {
      std::lock_guard<std::mutex> lk(mu_);
      return random_key(generator_);
    };

    std::mutex cerr_mu;
    ErrorSink error_sink =
        [&cerr_mu](std::vector<google::cloud::Status> const& errors) {
          std::lock_guard<std::mutex> lk(cerr_mu);
          for (auto const& e : errors) {
            std::cerr << "# " << e << "\n";
          }
        };

    std::vector<std::future<int>> tasks(thread_count);
    auto start = std::chrono::steady_clock::now();
    int task_id = 0;
    for (auto& t : tasks) {
      auto client = clients[task_id++ % clients.size()];
      t = std::async(std::launch::async, &SelectExperiment::RunTask, this,
                     config, client, locked_random_key, error_sink);
    }
    int total_count = 0;
    for (auto& t : tasks) {
      total_count += t.get();
    }
    auto elapsed = std::chrono::steady_clock::now() - start;

    sink({SingleRowThroughputSample{
        static_cast<int>(clients.size()), thread_count, total_count,
        std::chrono::duration_cast<std::chrono::microseconds>(elapsed)}});
  }

  int RunTask(Config const& config, cloud_spanner::Client client,
              RandomKeyGenerator const& key_generator,
              ErrorSink const& error_sink) {
    int count = 0;
    std::string value(1024, 'A');
    std::vector<google::cloud::Status> errors;
    for (auto start = std::chrono::steady_clock::now(),
              deadline = start + config.iteration_duration;
         start < deadline; start = std::chrono::steady_clock::now()) {
      auto key = key_generator();
      auto rows = client.ExecuteQuery(
          cloud_spanner::SqlStatement("SELECT Key, Data FROM KeyValue"
                                      " WHERE Key = @key",
                                      {{"key", cloud_spanner::Value(key)}}));
      for (auto& row :
           cloud_spanner::StreamOf<std::tuple<std::int64_t, std::string>>(
               rows)) {
        if (!row) {
          errors.push_back(std::move(row).status());
          break;
        }
        ++count;
      }
    }
    error_sink(std::move(errors));
    return count;
  }

 private:
  std::mutex mu_;
  google::cloud::internal::DefaultPRNG generator_;
};

class RunAllExperiment : public Experiment {
 public:
  void SetUp(Config const&, cloud_spanner::Database const&) override {}

  void Run(Config const& cfg, cloud_spanner::Database const& /*database*/,
           SampleSink const&) override {
    // Smoke test all the experiments by running a very small version of each.
    for (auto& kv : AvailableExperiments()) {
      // Do not recurse, skip this experiment.
      if (kv.first == "run-all") continue;
      Config config = cfg;
      config.table_size = 10;
      config.samples = 1;
      config.iteration_duration = std::chrono::seconds(1);
      std::cout << "# Smoke test for experiment: " << kv.first << "\n";
      // TODO(#1119) - tests disabled until we can stay within admin op quota
#if 0
      kv.second->SetUp(config, database);
      kv.second->Run(config, database, sink);
#endif
    }
  }
};

std::map<std::string, std::shared_ptr<Experiment>> AvailableExperiments() {
  return {
      {"run-all", std::make_shared<RunAllExperiment>()},
      {"insert-or-update", std::make_shared<InsertOrUpdateExperiment>()},
      {"read", std::make_shared<ReadExperiment>()},
      {"update", std::make_shared<UpdateDmlExperiment>()},
      {"select", std::make_shared<SelectExperiment>()},
  };
}

}  // namespace
