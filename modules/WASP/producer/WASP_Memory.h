using namespace caspar;

#include <core/producer/frame_producer.h>

struct OUTPUTINFO
{
	DWORD		dwWidth;
	DWORD		dwHeight;
	DWORD		dwBitCount;
	DWORD		dwFrameRate;
	DWORD		dwFirstField;
	BOOL		bInterlaced;
	BOOL		bIsNLE;
	DWORD		dwField;
	
	OUTPUTINFO()
	{
		dwWidth				= 0;
		dwHeight			= 0;
		dwBitCount			= 0;
		dwFrameRate			= 0;
		dwFirstField		= 0;
		bInterlaced			= FALSE;
		bIsNLE				= FALSE;
		dwField				= 0;
	}
	~OUTPUTINFO()
	{

	}
};

#include <queue>
using namespace std;


#define WASPWRITEEVENT L"WaspWriteData"
#define WASPREADEVENT  L"WaspReadData"
#define WASPOUTPUTFILE L"WaspMemoryOutputFile"
#define MAX_VIDEO_SIZE 720*576*4



class CWASP_Memory
{
public:

	CWASP_Memory()
	{
		m_hMappedFile	= NULL;
		m_hPipe			= NULL;
		//GetSharedMemoryHandles();
	}


BOOL GetSharedMemoryHandles();
short GetCheckSum(CHAR*);
BOOL CreateBufferPool();

BOOL  SendCommandToPipe(const std::wstring );
BOOL  ReadCommandFromPipe();
safe_ptr<core::basic_frame> receive(const safe_ptr<core::frame_factory> frame_factory_);
static DWORD WINAPI ReadThread(LPVOID);
void Readproc();

private:

	HANDLE   m_hWriteEvt;
	HANDLE	 m_hReadEvt;
	
	HANDLE			m_hMappedFile;
	HANDLE			m_hPipe;
	OUTPUTINFO		*m_pOutPut;
	BYTE			*m_pFileBuff;

	const std::wstring description_;
	core::monitor::subject				monitor_subject_;
	safe_ptr<core::basic_frame>			frame_;
	safe_ptr<core::basic_frame>			frame_1;
	safe_ptr<core::basic_frame>			last_frame;

	typedef queue<BYTE *>		tBufferQueue;
	tBufferQueue   m_lockedBuffers;
	tBufferQueue   m_freeBuffers;
	BOOL		   m_bRunning;
	DWORD		   m_dwSize;
	HANDLE		   m_hReadThread;

	CRITICAL_SECTION	 m_cslockBuffers;
	CRITICAL_SECTION	 m_csfreeBuffers;
};