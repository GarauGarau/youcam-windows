#include "recorder.h"
#include <winrt/Windows.Graphics.Capture.h>
#include <iostream>
#include <fstream>
using namespace youcam;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
std::shared_ptr<const Frame> awaitFrame(Frames& frames) {
    for (int i=0; i<250; ++i) { if (auto frame=frames.get()) return frame; Sleep(20); }
    throw std::runtime_error("No native capture frame within five seconds.");
}
void inspectMovie(const std::filesystem::path& path, UINT32 width, UINT32 height, bool audio) {
    ComPtr<IMFSourceReader> reader; check(MFCreateSourceReaderFromURL(path.c_str(),nullptr,&reader),"Open saved MP4");
    ComPtr<IMFMediaType> type; check(reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,&type),"Read saved video metadata");
    UINT32 actualW=0,actualH=0; check(MFGetAttributeSize(type.Get(),MF_MT_FRAME_SIZE,&actualW,&actualH),"Read saved video dimensions");
    require(actualW==width && actualH==height,"Encoded video lost source resolution.");
    if (audio) { ComPtr<IMFMediaType> sound; check(reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM,&sound),"Read saved microphone stream"); }
    ComPtr<IMFMediaType> decoded; MFCreateMediaType(&decoded); decoded->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Video); decoded->SetGUID(MF_MT_SUBTYPE,MFVideoFormat_NV12);
    check(reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,nullptr,decoded.Get()),"Configure native video decoder");
    reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS,FALSE); reader->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM,TRUE);
    int count=0;
    for (;;) { ComPtr<IMFSample> sample; DWORD flags=0; LONGLONG t=0;
        check(reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM,0,nullptr,&flags,&t,&sample),"Decode saved video");
        if (sample) ++count; if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
    }
    require(count>0,"Saved video contains no decoded frames.");
    std::cout << "PASS: encoded and decoded " << width << 'x' << height << (audio?" with AAC":"") << ".\n";
}
LRESULT CALLBACK fixtureProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m==WM_PAINT) { PAINTSTRUCT ps{}; HDC dc=BeginPaint(h,&ps); RECT r{};GetClientRect(h,&r); RECT half=r;half.right=r.right/2;
        HBRUSH red=CreateSolidBrush(RGB(255,0,0)),blue=CreateSolidBrush(RGB(0,0,255));FillRect(dc,&r,blue);FillRect(dc,&half,red);DeleteObject(red);DeleteObject(blue);EndPaint(h,&ps);return 0; }
    if (m==WM_DESTROY) { PostQuitMessage(0);return 0; } return DefWindowProcW(h,m,w,l);
}
int fixture() {
    WNDCLASSW c{};c.hInstance=GetModuleHandleW(nullptr);c.lpfnWndProc=fixtureProc;c.lpszClassName=L"YouCamTestFixture"; RegisterClassW(&c);
    RECT r{0,0,320,240};AdjustWindowRect(&r,WS_OVERLAPPEDWINDOW,FALSE);
    HWND window=CreateWindowW(c.lpszClassName,L"YouCam capture fixture",WS_OVERLAPPEDWINDOW|WS_VISIBLE,100,100,rectWidth(r),rectHeight(r),nullptr,nullptr,c.hInstance,nullptr);
    MSG m{};while(GetMessageW(&m,nullptr,0,0)>0){TranslateMessage(&m);DispatchMessageW(&m);}return 0;
}
struct Child {
    PROCESS_INFORMATION process{}; HWND window=nullptr;
    Child() {
        wchar_t executable[MAX_PATH];GetModuleFileNameW(nullptr,executable,MAX_PATH); std::wstring command=L"\""+std::wstring(executable)+L"\" --fixture";
        STARTUPINFOW startup{};startup.cb=sizeof(startup);
        require(CreateProcessW(nullptr,command.data(),nullptr,nullptr,FALSE,0,nullptr,nullptr,&startup,&process)!=FALSE,"Cannot start window fixture.");
        for(int i=0;i<100 && !window;++i){
            EnumWindows([](HWND h,LPARAM data)->BOOL{auto child=reinterpret_cast<Child*>(data);DWORD pid=0;GetWindowThreadProcessId(h,&pid);if(pid==child->process.dwProcessId && IsWindowVisible(h)){child->window=h;return FALSE;}return TRUE;},reinterpret_cast<LPARAM>(this));Sleep(50);
        }
        require(window!=nullptr,"Fixture did not create a visible window.");
    }
    ~Child(){TerminateProcess(process.hProcess,0);WaitForSingleObject(process.hProcess,5000);CloseHandle(process.hProcess);CloseHandle(process.hThread);}
};
int wmain(int argc,wchar_t**argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if(argc>1 && std::wstring(argv[1])==L"--fixture")return fixture();
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);check(MFStartup(MF_VERSION),"Initialize Media Foundation");
        auto directory=std::filesystem::temp_directory_path()/(L"YouCam native è '"+std::to_wstring(GetCurrentProcessId()));std::filesystem::create_directories(directory);
        for (auto size : {std::pair{640u,480u},std::pair{3840u,2160u}}) {
            Frame frame(size.first,size.second); for(size_t i=0;i<frame.bgra.size();i+=4){frame.bgra[i]=40;frame.bgra[i+1]=100;frame.bgra[i+2]=220;frame.bgra[i+3]=255;}
            bool audio=size.first==640;auto path=directory/(std::to_wstring(size.first)+L".mp4");Movie movie;movie.start(path,size.first,size.second,audio);
            for(int i=0;i<3;++i){movie.video(frame,int64_t(i)*10000000/30,10000000/30);if(audio)movie.audio(std::vector<int16_t>(1600,1200),int64_t(i)*10000000/30);}
            movie.finish();inspectMovie(path,size.first,size.second,audio);
        }
        require(RegisterHotKey(nullptr,11,MOD_CONTROL|MOD_ALT|MOD_SHIFT|MOD_NOREPEAT,'R')!=FALSE,"Cannot register native shortcut.");UnregisterHotKey(nullptr,11);
        if (winrt::Windows::Graphics::Capture::GraphicsCaptureSession::IsSupported()) {
            auto displays=monitors();require(!displays.empty(),"No native monitor.");auto m=displays.front();
            DesktopCapture screen;screen.start({SourceKind::Screen,m.handle,nullptr,m.rect,m.name});auto full=awaitFrame(*screen.frames);
            require(full->width==uint32_t(rectWidth(m.rect)) && full->height==uint32_t(rectHeight(m.rect)),"Screen capture is not in physical pixels.");screen.stop();
            RECT crop{m.rect.left+20,m.rect.top+30,m.rect.left+181,m.rect.top+151};
            DesktopCapture region;region.start({SourceKind::Region,m.handle,nullptr,crop,L"Test area"});auto cropped=awaitFrame(*region.frames);
            require(cropped->width==161 && cropped->height==121,"Region capture changed native dimensions.");region.stop();
            Child child;DesktopCapture window;window.start({SourceKind::Window,nullptr,child.window,{},L"Fixture"});auto image=awaitFrame(*window.frames);
            require(image->width==320 && image->height==240,"Window client area capture has wrong dimensions.");
            require(image->bgra[(120*320+80)*4+2]>240 && image->bgra[(120*320+240)*4]>240,"Window capture has wrong colors.");window.stop();
            auto path=directory/L"pause.mp4";Recorder recorder;recorder.start({SourceKind::Window,nullptr,child.window,{},L"Fixture"},nullptr,false,path);
            for(int i=0;i<250 && recorder.state().phase==Phase::Starting;++i)Sleep(20);
            require(recorder.state().phase==Phase::Recording,narrow(L"Recording startup: "+wide(recorder.state().error)).c_str());Sleep(300);
            ShowWindow(child.window,SW_MINIMIZE);
            for(int i=0;i<100 && recorder.state().phase!=Phase::Paused;++i)Sleep(20);
            require(recorder.state().phase==Phase::Paused,"Minimized window did not pause recording.");
            auto paused=recorder.state().elapsed;Sleep(100);require(recorder.state().elapsed==paused,"Paused time advanced.");
            ShowWindow(child.window,SW_RESTORE);Sleep(200);recorder.resume();Sleep(300);recorder.stop();
            require(recorder.state().error.empty(),recorder.state().error.c_str());inspectMovie(path,320,240,false);
            std::cout<<"PASS: physical monitor pixels, exact region pixels, native window capture, minimize/pause/resume.\n";
        } else std::cout<<"SKIP: Windows Graphics Capture is unavailable on this runner; desktop checks require an active Windows 11 session.\n";
        std::filesystem::remove_all(directory);MFShutdown();winrt::uninit_apartment();return 0;
    }catch(const winrt::hresult_error& error){std::cerr<<narrow(error.message().c_str())<<'\n';return 1;}
    catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
