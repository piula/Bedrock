#pragma once
#include <libstuff/libstuff.h>
#include "../BedrockPlugin.h"
#include "../BedrockServer.h"
#include "../libs/S3.h"


class BedrockPlugin_BackupManager : public BedrockPlugin {
    public:
        // Constructor
        BedrockPlugin_BackupManager();

        // Destructor
        ~BedrockPlugin_BackupManager();

        // Initialize our plugin, sets up our HTTPSManager.
        void initialize(const SData& args, BedrockServer& server);

        bool peekCommand(SQLite& db, BedrockCommand& command);

        virtual string getName() { return "backupManager"; }

        virtual bool preventAttach();

        static bool serverDetached();

    private:

        // Our BedrockServer Instance
        BedrockServer* _server;

        static SData localArgs;
        static SData keys;

        // Instance of ourselves.
        static BedrockPlugin_BackupManager* _instance;

        // Used to store details for backup/restores.
        static STable details;

        // Used to prevent two backups from running at the same time.
        static bool operationInProgress;
        static mutex operationMutex;

        // Mutex to wrap our fileMainfest. Necessary because any thread could be
        // attempting to modify the manifest at any given time.
        static mutex fileManifestMutex;

        // An STable containing all of our file piece and details (size and offset)
        // for each piece. For uploads we append to this manifest in each thread
        // then turn it into JSON when we call _saveManifest. For downloads
        // we read this STable out of the downloaded manifest and use it to
        // download the correct files and know the given details for each file.
        static STable fileManifest;

        // Wrapper function that spawns the upload worker threads. Detaches the
        // database by sending a `Detach` command to bedrock. Once we're done it
        // sends an `Attach` command, to let bedrock know we're done.
        static void _beginBackup(BedrockPlugin_BackupManager* plugin, bool exitWhenComplete = false);

        // Wrapper function that spawns our restore worker threads. Owns the single
        // file pointer for writing out out database.
        static void _beginRestore(BedrockPlugin_BackupManager* plugin, bool exitWhenComplete = false);

        // Internal function for downloading the JSON manifest from S3 and
        // starting a bootstrap.
        static void _downloadManifest();

        // Internal function for generating the JSON manifest file for the backup.
        static void _saveManifest();

        // Wrapper function to loop over our wrapper functions in a thread.
        static void _poll(S3& s3, SHTTPSManager::Transaction* request);

        // Wrappers for this plugin that just call the base class of the HTTPSManager.
        static void _prePoll(fd_map& fdm, S3& s3);
        static void _postPoll(fd_map& fdm, uint64_t nextActivity, S3& s3);

        // Internal function to download, gunzip, and decrypt a given file from
        // the manifest. This function is called in worker threads so all operations
        // need to be thread safe.
        static string _processFileChunkDownload(const string& fileName, size_t& fileSize, size_t& gzippedFileSize, S3& s3, string fileHash);

        // Internal function to encrypt, gzip, and upload a given file chunk from
        // the database. Starts by creating a multipart upload in S3, then chunking
        // the file into 5MB pieces and uploading each one as a "part" of the database
        // chunk. Once it's finished it "finishes" the multipart upload which causes
        // S3 to put all of the pieces together on their end. Finally, it adds the
        // file details to the fileManifest. This function is called in worker threads
        // so all operations need to be thread safe.
        static void _processFileChunkUpload(char* fileChunk, size_t& fromSize,
                                            size_t& chunkOffset, const string& chunkNumber, S3& s3);

        // Lets a thread tell all the others that it's broken and everyone should exit.
        static atomic<bool> shouldExit;

        static bool _isZero(const char* c, uint64_t size);

};
