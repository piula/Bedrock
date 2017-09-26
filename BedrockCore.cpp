#include "BedrockCore.h"
#include "BedrockPlugin.h"
#include "BedrockServer.h"

BedrockCore::BedrockCore(SQLite& db, const BedrockServer& server) : 
SQLiteCore(db),
_server(server)
{ }

bool BedrockCore::peekCommand(BedrockCommand& command) {
    AutoTimer timer(command, BedrockCommand::PEEK);
    // Convenience references to commonly used properties.
    SData& request = command.request;
    SData& response = command.response;
    STable& content = command.jsonContent;
    SDEBUG("Peeking at '" << request.methodLine << "'");
    command.peekCount++;

    // We catch any exception and handle in `_handleCommandException`.
    try {
        try {
            _db.startTiming(5'000'000);
            // We start a transaction in `peekCommand` because we want to support having atomic transactions from peek
            // through process. This allows for consistency through this two-phase process. I.e., anything checked in
            // peek is guaranteed to still be valid in process, because they're done together as one transaction.
            if (!_db.beginConcurrentTransaction()) {
                STHROW("501 Failed to begin concurrent transaction");
            }

            // Try each plugin, and go with the first one that says it succeeded.
            bool pluginPeeked = false;
            for (auto plugin : _server.plugins) {
                // Try to peek the command.
                if (plugin->peekCommand(_db, command)) {
                    SINFO("Plugin '" << plugin->getName() << "' peeked command '" << request.methodLine << "'");
                    pluginPeeked = true;
                    break;
                }
            }

            // If nobody succeeded in peeking it, then we'll need to process it.
            // TODO: Would be nice to be able to check if a plugin *can* handle a command, so that we can differentiate
            // between "didn't peek" and "peeked but didn't complete".
            if (!pluginPeeked) {
                SINFO("Command '" << request.methodLine << "' is not peekable, queuing for processing.");
                _db.resetTiming();
                return false;
            }

            // If no response was set, assume 200 OK
            if (response.methodLine == "") {
                response.methodLine = "200 OK";
            }

            // Add the commitCount header to the response.
            response["commitCount"] = to_string(_db.getCommitCount());

            // Success. If a command has set "content", encode it in the response.
            SINFO("Responding '" << response.methodLine << "' to read-only '" << request.methodLine << "'.");
            if (!content.empty()) {
                // Make sure we're not overwriting anything different.
                string newContent = SComposeJSONObject(content);
                if (response.content != newContent) {
                    if (!response.content.empty()) {
                        SWARN("Replacing existing response content in " << request.methodLine);
                    }
                    response.content = newContent;
                }
            }
        } catch (const SQLite::timeout_error& e) {
            STHROW("555 Timeout peeking command");
        }
    } catch (const SException& e) {
        _handleCommandException(command, e, false);
    }

    // If we get here, it means the command is fully completed.
    command.complete = true;

    // Back out of the current transaction, it doesn't need to do anything.
    _db.rollback();
    _db.resetTiming();

    // Done.
    return true;
}

bool BedrockCore::processCommand(BedrockCommand& command) {
    AutoTimer timer(command, BedrockCommand::PROCESS);

    // Convenience references to commonly used properties.
    SData& request = command.request;
    SData& response = command.response;
    STable& content = command.jsonContent;
    SDEBUG("Processing '" << request.methodLine << "'");
    command.processCount++;

    // Keep track of whether we've modified the database and need to perform a `commit`.
    bool needsCommit = false;
    try {
        try {
            // Time in US.
            _db.startTiming(5'000'000);
            // If a transaction was already begun in `peek`, then this is a no-op. We call it here to support the case where
            // peek created a httpsRequest and closed it's first transaction until the httpsRequest was complete, in which
            // case we need to open a new transaction.
            if (!_db.insideTransaction() && !_db.beginConcurrentTransaction()) {
                STHROW("501 Failed to begin concurrent transaction");
            }

            // Loop across the plugins to see which wants to take this.
            bool pluginProcessed = false;
            for (auto plugin : _server.plugins) {
                // Try to process the command.
                if (plugin->processCommand(_db, command)) {
                    SINFO("Plugin '" << plugin->getName() << "' processed command '" << request.methodLine << "'");
                    pluginProcessed = true;
                    break;
                }
            }

            // If no plugin processed it, respond accordingly.
            if (!pluginProcessed) {
                SWARN("Command '" << request.methodLine << "' does not exist.");
                STHROW("430 Unrecognized command");
            }

            // If we have no uncommitted query, just rollback the empty transaction. Otherwise, we need to commit.
            if (_db.getUncommittedQuery().empty()) {
                _db.rollback();
            } else {
                needsCommit = true;
            }

            // If no response was set, assume 200 OK
            if (response.methodLine == "") {
                response.methodLine = "200 OK";
            }

            // Add the commitCount header to the response.
            response["commitCount"] = to_string(_db.getCommitCount());

            // Success, this command will be committed.
            SINFO("Processed '" << response.methodLine << "' for '" << request.methodLine << "'.");

            // Finally, if a command has set "content", encode it in the response.
            if (!content.empty()) {
                // Make sure we're not overwriting anything different.
                string newContent = SComposeJSONObject(content);
                if (response.content != newContent) {
                    if (!response.content.empty()) {
                        SWARN("Replacing existing response content in " << request.methodLine);
                    }
                    response.content = newContent;
                }
            }
        } catch (const SQLite::timeout_error& e) {
            STHROW("555 Timeout processing command");
        }
    } catch (const SException& e) {
        _handleCommandException(command, e, true);
    }

    _db.resetTiming();

    // Done, return whether or not we need the parent to commit our transaction.
    command.complete = !needsCommit;
    return needsCommit;
}

void BedrockCore::_handleCommandException(BedrockCommand& command, const SException& e, bool wasProcessing) {
    // If we were peeking, then we weren't in a transaction. But if we were processing, we need to roll it back.
    //if (wasProcessing) {
        _db.rollback();
    //}
    _db.resetTiming();
    const string& msg = "Error processing command '" + command.request.methodLine + "' (" + e.what() + "), ignoring: " +
                        command.request.serialize();
    if (SContains(e.what(), "_ALERT_")) {
        SALERT(msg);
    } else if (SContains(e.what(), "_WARN_")) {
        SWARN(msg);
    } else if (SContains(e.what(), "_HMMM_")) {
        SHMMM(msg);
    } else if (SStartsWith(e.what(), "50")) {
        SALERT(msg); // Alert on 500 level errors.
    } else {
        SINFO(msg);
    }

    // Set the response to the values from the exception, if set.
    if (!e.method.empty()) {
        command.response.methodLine = e.method;
    }
    if (!e.headers.empty()) {
        command.response.nameValueMap = e.headers;
    }
    if (!e.body.empty()) {
        command.response.content = e.body;
    }

    // Add the commitCount header to the response.
    command.response["commitCount"] = to_string(_db.getCommitCount());
}
