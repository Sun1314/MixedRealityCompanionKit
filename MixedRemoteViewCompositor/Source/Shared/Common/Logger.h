#pragma once

struct __declspec(uuid("5cb90ac0-a48a-411d-be3b-a2ac29f09f00")) ILogger : public ::IUnknown
{
    STDMETHOD(Log)(_In_ HSTRING message) = 0;
};

class LoggerImpl
    : public Microsoft::WRL::RuntimeClass
    < Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::RuntimeClassType::WinRtClassicComMix>
    , ILogger
    , Microsoft::WRL::FtmBase >
{
public:
    LoggerImpl();
    virtual ~LoggerImpl();

    IFACEMETHOD(Log)(_In_ HSTRING message);

    static ILogger* Instance();

    HRESULT RuntimeClassInitialize(_In_ LPCWSTR wcsFilename);

protected:
    HRESULT OnFileCreated(_In_ ABI::Windows::Foundation::IAsyncOperation<ABI::Windows::Storage::StorageFile*>* asyncOp, _In_ AsyncStatus status);
    HRESULT OnFileOpened(_In_ ABI::Windows::Foundation::IAsyncOperation<ABI::Windows::Storage::Streams::IRandomAccessStream*>* asyncOp, _In_ AsyncStatus status);

private:
    HRESULT FlushMessageQueue();

private:
    static Microsoft::WRL::ComPtr<ILogger> s_spInstance;

    Microsoft::WRL::Wrappers::CriticalSection _lock;

    boolean m_savingToFile;
    std::vector<std::wstring> m_logMessages;

    Microsoft::WRL::ComPtr<ABI::Windows::Storage::Streams::IOutputStream> m_spOutputStream;
    Microsoft::WRL::ComPtr<ABI::Windows::Storage::Streams::IDataWriter> m_spDataWriter;
};

