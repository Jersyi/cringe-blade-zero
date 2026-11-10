#include "append_log.h"
#include <filesystem>
#include <fstream>
#include <set>
#include <thread>
#include <vector>
#include <cstdio>
int main(){
    namespace fs=std::filesystem;int checks=0,failures=0;const auto check=[&](bool ok){++checks;if(!ok)++failures;};
    const auto root=fs::temp_directory_path()/(L"pbz_log_test_"+std::to_wstring(GetCurrentProcessId())+L"_"+std::to_wstring(GetTickCount64()));fs::create_directory(root);
    const auto path=root/L"test.log";AppendLog log;std::vector<std::thread> threads;LONG write_failures=0;
    for(int t=0;t<8;++t)threads.emplace_back([&,t]{for(int i=0;i<500;++i){const auto line=std::to_string(t)+":"+std::to_string(i)+"\n";if(!log.Write(path.wstring(),line.data(),static_cast<DWORD>(line.size())))InterlockedIncrement(&write_failures);}});
    for(auto& thread:threads)thread.join();log.Close();check(write_failures==0);
    std::ifstream input(path);std::set<std::string> lines;std::string line;while(std::getline(input,line))lines.insert(line);input.close();check(lines.size()==4000);
    for(int t=0;t<8;++t)for(int i=0;i<500;++i)check(lines.count(std::to_string(t)+":"+std::to_string(i))==1);
    const auto rotate=root/L"rotate.log";AppendLog small(4);check(small.Write(rotate.wstring(),"one\n",4));check(small.Write(rotate.wstring(),"two\n",4));small.Close();
    std::ifstream prev(rotate.wstring()+L".previous");std::getline(prev,line);check(line=="one");prev.close();
    std::ifstream last(rotate);std::getline(last,line);check(line=="two");last.close();
    check(!log.Write((root/L"absent"/L"bad.log").wstring(),"x",1));check(log.Write(path.wstring(),"recovered\n",10));log.Close();
    for(const auto& file:fs::directory_iterator(root))if(file.is_regular_file())fs::remove(file.path());fs::remove(root);
    std::printf("{\"checks\":%d,\"failures\":%d,\"threads\":8,\"lines\":4000}\n",checks,failures);return failures?1:0;
}
