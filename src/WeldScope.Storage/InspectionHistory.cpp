#include "InspectionHistory.h"

#include <mysql.h>
#include <errmsg.h>
#include <mysqld_error.h>

#include <array>
#include <charconv>
#include <cstdlib>
#include <limits>
#include <memory>
#include <string_view>
#include <tuple>
#include <utility>

namespace weldscope {
namespace {

constexpr std::string_view insertInspection =
    "INSERT INTO inspections (inspection_id, request_id, input_id, source_path, source_sha256, config_sha256, "
    "algorithm_version, dataset_version, decision, reason, work_state) "
    "VALUES (?, ?, ?, ?, NULLIF(?, ''), NULLIF(?, ''), ?, ?, NULLIF(?, ''), ?, ?)";
constexpr std::string_view insertAttempt =
    "INSERT INTO analysis_attempts (inspection_id, sequence_no, stage, score, decision, reason, duration_ms) "
    "VALUES (?, ?, ?, ?, ?, ?, ?)";

std::string Environment(const char* name, const char* fallback = "") {
    const char* value = std::getenv(name);
    return value ? value : fallback;
}

class DatabaseError : public std::runtime_error {
public:
    DatabaseError(const char* operation, unsigned int code, const char* detail)
        : std::runtime_error(std::string(operation) + " [MySQL " + std::to_string(code) + "] " + detail),
          code(code) {}

    const unsigned int code;
};

using Connection = std::unique_ptr<MYSQL, decltype(&mysql_close)>;

void InitializeClient() {
    // C++ serializes function-local static initialization across first callers.
    static const struct Library {
        Library() {
            if (mysql_library_init(0, nullptr, nullptr) != 0) {
                throw std::runtime_error("MySQL library initialization failed");
            }
        }
        ~Library() { mysql_library_end(); }
    } library;
    thread_local const struct Thread {
        Thread() {
            if (mysql_thread_init()) throw std::runtime_error("MySQL thread initialization failed");
        }
        ~Thread() { mysql_thread_end(); }
    } thread;
}

void CheckConnection(MYSQL* connection, bool success, const char* operation) {
    if (!success) throw DatabaseError(operation, mysql_errno(connection), mysql_error(connection));
}

Connection Connect(const HistoryConnection& settings) {
    const bool isLoopback = settings.host == "127.0.0.1" || settings.host == "::1" || settings.host == "localhost";
    if (!isLoopback && settings.caFile.empty()) {
        throw std::invalid_argument("A MySQL CA file is required for remote connections");
    }
    InitializeClient();
    Connection connection(mysql_init(nullptr), mysql_close);
    if (!connection) throw std::runtime_error("MySQL initialization failed");
    auto* mysql = connection.get();
    constexpr unsigned int timeoutSeconds = 5;
    constexpr unsigned int protocol = MYSQL_PROTOCOL_TCP;
    const auto option = [mysql](mysql_option name, const void* value) {
        CheckConnection(mysql, mysql_options(mysql, name, value) == 0, "Connection option");
    };
    option(MYSQL_OPT_PROTOCOL, &protocol);
    option(MYSQL_OPT_CONNECT_TIMEOUT, &timeoutSeconds);
    option(MYSQL_OPT_READ_TIMEOUT, &timeoutSeconds);
    option(MYSQL_OPT_WRITE_TIMEOUT, &timeoutSeconds);
    option(MYSQL_SET_CHARSET_NAME, "utf8mb4");
    option(MYSQL_INIT_COMMAND, "SET SESSION sql_mode='STRICT_ALL_TABLES,NO_ENGINE_SUBSTITUTION'");
    if (!settings.caFile.empty()) {
        constexpr unsigned int sslMode = SSL_MODE_VERIFY_IDENTITY;
        option(MYSQL_OPT_SSL_CA, settings.caFile.c_str());
        option(MYSQL_OPT_SSL_MODE, &sslMode);
    }
    if (!mysql_real_connect(mysql, settings.host.c_str(), settings.user.c_str(), settings.password.c_str(),
                            settings.database.c_str(), settings.port, nullptr, 0)) {
        // Avoid including server-supplied connection text in logs that may contain credentials.
        throw std::runtime_error("Connection failed [MySQL " + std::to_string(mysql_errno(mysql)) + "]: " +
                                 mysql_sqlstate(mysql));
    }
    return connection;
}

class Transaction {
public:
    explicit Transaction(MYSQL* connection) : connection_(connection) {
        CheckConnection(connection_, mysql_query(connection_, "START TRANSACTION") == 0, "Start transaction");
    }

    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;

    ~Transaction() {
        if (active_) mysql_rollback(connection_);
    }

    void Rollback() {
        CheckConnection(connection_, mysql_rollback(connection_) == 0, "Rollback");
        active_ = false;
    }

    void Commit() {
        if (mysql_commit(connection_) != 0) {
            const auto code = mysql_errno(connection_);
            const DatabaseError error("Commit", code, mysql_error(connection_));
            if (code == CR_SERVER_GONE_ERROR || code == CR_SERVER_LOST || code == CR_SERVER_LOST_EXTENDED) {
                throw CommitOutcomeUnknown(error.what());
            }
            throw error;
        }
        active_ = false;
    }

private:
    MYSQL* connection_;
    bool active_ = true;
};

MYSQL_BIND Parameter(const std::string& value) {
    if (value.size() > (std::numeric_limits<unsigned long>::max)()) {
        throw std::invalid_argument("MySQL parameter is too large");
    }
    MYSQL_BIND bind{};
    bind.buffer_type = MYSQL_TYPE_STRING;
    bind.buffer = const_cast<char*>(value.data());
    bind.buffer_length = static_cast<unsigned long>(value.size());
    return bind;
}

MYSQL_BIND Parameter(const unsigned int& value) {
    MYSQL_BIND bind{};
    bind.buffer_type = MYSQL_TYPE_LONG;
    bind.buffer = const_cast<unsigned int*>(&value);
    bind.is_unsigned = true;
    return bind;
}

MYSQL_BIND Parameter(const double& value) {
    MYSQL_BIND bind{};
    bind.buffer_type = MYSQL_TYPE_DOUBLE;
    bind.buffer = const_cast<double*>(&value);
    return bind;
}

MYSQL_BIND Parameter(const std::optional<double>& value) {
    if (value) return Parameter(*value);
    MYSQL_BIND bind{};
    bind.buffer_type = MYSQL_TYPE_NULL;
    return bind;
}

struct Column {
    explicit Column(std::string& value) : text(&value) { bind.buffer_type = MYSQL_TYPE_STRING; }
    explicit Column(double& value) {
        bind.buffer_type = MYSQL_TYPE_DOUBLE;
        bind.buffer = &value;
    }
    explicit Column(std::optional<double>& value) : nullableNumber(&value) { bind.buffer_type = MYSQL_TYPE_DOUBLE; }

    MYSQL_BIND Bind() {
        bind.is_null = &isNull;
        bind.error = &hasError;
        bind.length = &length;
        if (nullableNumber) bind.buffer = &number;
        return bind;
    }

    MYSQL_BIND bind{};
    std::string* text = nullptr;
    std::optional<double>* nullableNumber = nullptr;
    double number = 0;
    unsigned long length = 0;
    bool isNull = false;
    bool hasError = false;
};

class Statement {
public:
    Statement(MYSQL* connection, const char* operation, std::string_view sql)
        : statement_(mysql_stmt_init(connection), mysql_stmt_close), operation_(operation) {
        if (!statement_) throw std::runtime_error("MySQL statement initialization failed");
        Check(mysql_stmt_prepare(statement_.get(), sql.data(), static_cast<unsigned long>(sql.size())) == 0);
    }

    template<class... Values>
    void Execute(const Values&... values) {
        auto* statement = statement_.get();
        if (mysql_stmt_param_count(statement) != sizeof...(values)) throw std::logic_error("SQL parameter count mismatch");
        if constexpr (sizeof...(values) != 0) {
            std::array<MYSQL_BIND, sizeof...(values)> bindings{Parameter(values)...};
            Check(mysql_stmt_bind_param(statement, bindings.data()) == 0);
            Check(mysql_stmt_execute(statement) == 0);
        } else {
            Check(mysql_stmt_execute(statement) == 0);
        }
    }

    template<class... Values>
    void ExecuteWrite(MYSQL* connection, const Values&... values) {
        Execute(values...);
        // Strict SQL mode can still accept a write with excess trailing spaces removed.
        if (mysql_warning_count(connection) != 0) {
            throw std::runtime_error(std::string(operation_) + " produced MySQL warnings");
        }
    }

    template<class... Values>
    bool Fetch(Values&... values) {
        auto* statement = statement_.get();
        std::array<Column, sizeof...(values)> columns{Column(values)...};
        std::array<MYSQL_BIND, sizeof...(values)> bindings{};
        if (mysql_stmt_field_count(statement) != columns.size()) throw std::logic_error("SQL result column count mismatch");
        for (std::size_t index = 0; index < columns.size(); ++index) bindings[index] = columns[index].Bind();
        Check(mysql_stmt_bind_result(statement, bindings.data()) == 0);
        const int result = mysql_stmt_fetch(statement);
        if (result == MYSQL_NO_DATA) return false;
        Check(result == 0 || result == MYSQL_DATA_TRUNCATED);

        for (unsigned int index = 0; index < columns.size(); ++index) {
            auto& column = columns[index];
            if (column.isNull && !column.nullableNumber) throw std::runtime_error("Unexpected SQL NULL");
            if (column.text) {
                // Fetch the full byte length for UTF-8 paths and reasons without fixed-size buffers.
                column.text->resize(column.length);
                if (column.length != 0) {
                    auto& bind = bindings[index];
                    bind.buffer = column.text->data();
                    bind.buffer_length = column.length;
                    Check(mysql_stmt_fetch_column(statement, &bind, index, 0) == 0);
                }
            } else {
                if (column.hasError) throw std::runtime_error("Invalid numeric SQL result");
                if (column.nullableNumber) {
                    *column.nullableNumber = column.isNull ? std::nullopt : std::optional<double>(column.number);
                }
            }
        }
        return true;
    }

private:
    void Check(bool success) const {
        if (!success) throw DatabaseError(operation_, mysql_stmt_errno(statement_.get()), mysql_stmt_error(statement_.get()));
    }

    std::unique_ptr<MYSQL_STMT, decltype(&mysql_stmt_close)> statement_;
    const char* operation_;
};

auto RecordFields(const InspectionRecord& record) {
    return std::tie(record.inspectionId, record.requestId, record.inputId, record.sourcePath, record.sourceSha256,
                    record.configSha256, record.algorithmVersion, record.datasetVersion, record.decision, record.reason,
                    record.attempts, record.workState);
}

std::optional<StoredInspection> ReadInspection(MYSQL* mysql, const std::string& inspectionId) {
    StoredInspection stored;
    auto& record = stored.record;
    record.inspectionId = inspectionId;
    {
        std::string workState;
        Statement inspection(mysql, "Read inspection",
            "SELECT request_id, input_id, source_path, COALESCE(source_sha256, ''), COALESCE(config_sha256, ''), "
            "algorithm_version, dataset_version, COALESCE(decision, ''), reason, created_at, work_state "
            "FROM inspections WHERE inspection_id = ?");
        inspection.Execute(inspectionId);
        if (!inspection.Fetch(record.requestId, record.inputId, record.sourcePath, record.sourceSha256,
                              record.configSha256, record.algorithmVersion, record.datasetVersion, record.decision,
                              record.reason, stored.createdAtUtc, workState)) return std::nullopt;
        if (workState == "CANCELLED") record.workState = InspectionWorkState::Cancelled;
    }
    Statement attempts(mysql, "Read analysis attempts",
        "SELECT stage, score, decision, reason, duration_ms FROM analysis_attempts "
        "WHERE inspection_id = ? ORDER BY sequence_no");
    attempts.Execute(inspectionId);
    AnalysisAttempt attempt{};
    while (attempts.Fetch(attempt.stage, attempt.score, attempt.decision, attempt.reason, attempt.durationMs)) {
        record.attempts.push_back(std::move(attempt));
    }
    return stored;
}

void ValidateId(const std::string& id) {
    if (id.empty() || id.size() > 64) throw std::invalid_argument("IDs must have 1 to 64 bytes");
}

void ValidateSha256(const std::string& hash) {
    if (hash.size() != 64 || hash.find_first_not_of("0123456789abcdef") != std::string::npos) {
        throw std::invalid_argument("SHA-256 values must have 64 lowercase hexadecimal digits");
    }
}

} // namespace

HistoryConnection HistoryConnectionFromEnvironment() {
    const auto portText = Environment("WELDSCOPE_MYSQL_PORT", "3309");
    unsigned int port = 0;
    const auto end = portText.data() + portText.size();
    const auto parsed = std::from_chars(portText.data(), end, port);
    if (parsed.ec != std::errc{} || parsed.ptr != end || port == 0 || port > 65535)
        throw std::invalid_argument("Invalid WELDSCOPE_MYSQL_PORT");
    auto password = Environment("WELDSCOPE_MYSQL_PASSWORD");
    if (password.empty()) throw std::invalid_argument("WELDSCOPE_MYSQL_PASSWORD is required");
    return {Environment("WELDSCOPE_MYSQL_HOST", "10.0.2.2"), port,
            Environment("WELDSCOPE_MYSQL_DATABASE", "weldscope_s05"),
            Environment("WELDSCOPE_MYSQL_USER", "weldscope_app"), std::move(password),
            Environment("WELDSCOPE_MYSQL_CA_FILE")};
}

bool AnalysisAttempt::operator==(const AnalysisAttempt& other) const {
    return std::tie(stage, score, decision, reason, durationMs) ==
           std::tie(other.stage, other.score, other.decision, other.reason, other.durationMs);
}

InspectionHistory::InspectionHistory(HistoryConnection settings) : settings_(std::move(settings)) {}

std::string InspectionHistory::Version() const {
    const auto connection = Connect(settings_);
    Statement version(connection.get(), "Read server version", "SELECT VERSION()");
    version.Execute();
    std::string serverVersion;
    if (!version.Fetch(serverVersion)) throw std::runtime_error("MySQL returned no server version");
    return serverVersion;
}

SaveStatus InspectionHistory::Save(const InspectionRecord& record) const {
    ValidateId(record.inspectionId);
    ValidateId(record.requestId);
    const bool cancelled = record.workState == InspectionWorkState::Cancelled;
    if (cancelled && (!record.decision.empty() || !record.attempts.empty()))
        throw std::invalid_argument("Cancellation cannot have a verdict or attempts");
    if (!record.sourceSha256.empty() || (!cancelled && record.decision != "ERROR"))
        ValidateSha256(record.sourceSha256);
    if (!record.configSha256.empty() || (!cancelled && record.decision != "ERROR"))
        ValidateSha256(record.configSha256);
    if (record.attempts.size() > (std::numeric_limits<unsigned int>::max)()) {
        throw std::invalid_argument("Too many analysis attempts");
    }
    const auto connection = Connect(settings_);
    auto* mysql = connection.get();
    Transaction transaction(mysql);
    try {
        Statement insert(mysql, "Insert inspection", insertInspection);
        insert.ExecuteWrite(mysql, record.inspectionId, record.requestId, record.inputId, record.sourcePath,
                            record.sourceSha256, record.configSha256, record.algorithmVersion,
                            record.datasetVersion, record.decision, record.reason,
                            std::string(cancelled ? "CANCELLED" : "COMPLETED"));
    } catch (const DatabaseError& error) {
        if (error.code != ER_DUP_ENTRY) throw;
        transaction.Rollback();
        const auto existing = ReadInspection(mysql, record.inspectionId);
        // A matching request_id on this row also proves ownership of the unique request key.
        return existing && RecordFields(existing->record) == RecordFields(record) ? SaveStatus::Replay : SaveStatus::Conflict;
    }
    if (!record.attempts.empty()) {
        Statement insert(mysql, "Insert analysis attempt", insertAttempt);
        unsigned int sequence = 0;
        for (const auto& attempt : record.attempts) {
            insert.ExecuteWrite(mysql, record.inspectionId, ++sequence, attempt.stage, attempt.score,
                                attempt.decision, attempt.reason, attempt.durationMs);
        }
    }
    transaction.Commit();
    return SaveStatus::Stored;
}

std::optional<StoredInspection> InspectionHistory::Get(const std::string& inspectionId) const {
    ValidateId(inspectionId);
    const auto connection = Connect(settings_);
    return ReadInspection(connection.get(), inspectionId);
}

void InspectionHistory::CheckReady() const {
    const auto connection = Connect(settings_);
    Statement(connection.get(), "Check inspection schema", "SELECT work_state FROM inspections WHERE FALSE").Execute();
    Statement(connection.get(), "Check attempt schema", "SELECT sequence_no FROM analysis_attempts WHERE FALSE").Execute();
    Statement(connection.get(), "Check inspection writer", insertInspection);
    Statement(connection.get(), "Check attempt writer", insertAttempt);
}

std::optional<StoredInspection> InspectionHistory::GetByRequestId(const std::string& requestId) const {
    ValidateId(requestId);
    const auto connection = Connect(settings_);
    std::string inspectionId;
    {
        Statement lookup(connection.get(), "Find request", "SELECT inspection_id FROM inspections WHERE request_id = ?");
        lookup.Execute(requestId);
        if (!lookup.Fetch(inspectionId)) return std::nullopt;
    }
    return ReadInspection(connection.get(), inspectionId);
}

std::vector<InspectionSummary> InspectionHistory::Recent() const {
    const auto connection = Connect(settings_);
    Statement list(connection.get(), "List inspections",
        "SELECT inspection_id, source_path, COALESCE(decision, ''), created_at, work_state "
        "FROM inspections ORDER BY created_at DESC, inspection_id DESC LIMIT 50");
    list.Execute();
    std::vector<InspectionSummary> records;
    InspectionSummary row;
    std::string state;
    while (list.Fetch(row.inspectionId, row.sourcePath, row.decision, row.createdAtUtc, state)) {
        row.workState = state == "CANCELLED" ? InspectionWorkState::Cancelled : InspectionWorkState::Completed;
        records.push_back(std::move(row));
    }
    return records;
}

} // namespace weldscope
