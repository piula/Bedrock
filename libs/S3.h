#pragma once
#include <libstuff/SHTTPSManager.h>

class S3 : public SHTTPSManager
{

public:
    S3(const string& awsAccessKey, const string& awsSecretKey,  const string& awsBucketName, bool live);
    virtual ~S3();

    // Get a given object from S3
    virtual Transaction* download(const string& fileName);

    // Add a given object to S3
    virtual Transaction* upload(const string& fileName, const string& file);

    // The test keys to use to make S3 requests when testing.
    static constexpr auto TEST_AWS_ACESS_KEY = "KEY";
    static constexpr auto TEST_AWS_SECRET_KEY = "SECRET";
    static constexpr auto TEST_AWS_BUCKET_NAME = "NEW BUCKET";

private:
    static constexpr auto S3_URL = "s3.amazonaws.com";

    // The keys to use to make an S3 request.
    map<string, string> _keys;

    // This is a map of our current file names in flight and the transactions
    // associated with them.
    map<string, string> _activeFileNameTransactions;

    // Mutex to lock around our map of file names and transactions.
    mutex _activeFileNameTransactionsMutex;

    const bool _live;
    const string BUCKET_URL;

    // SHTTPSManager operations are thread-safe, we lock around any accesses to our transaction lists, so that
    // multiple threads can add/remove from them.
    recursive_mutex _listMutex;

    string _getExceptionName();
    SData _buildRequest(const string& method, const string& fileName, const string& file="");
    STable _signRequest(const SData& request, const string& awsSecretKey, const string& scope);
    string _composeS3HTTP(const SData& request);
    SHTTPSManager::Transaction* _httpsSendRawData(const string& url, const SData& request);
    virtual Transaction* _sendRequest(const string& method, const string& fileName, const string& file="");

};
