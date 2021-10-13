#include "fileSystem.h"

#include <algorithm>

#ifdef _WIN32
#include <direct.h>
#define chdir _chdir
#define getcwd _getcwd
#else
#include "unistd.h"
#include <sys/stat.h>
#endif

using namespace std;



namespace fs
{

// TODO: free at 'finalize'
static unordered_map<std::string, das::TextFileInfo *> daslib_inc_files;


void initialize()
{
#include "resources/daslib_str/daslib_init.cpp.inl"
}


bool read_whole_file(const char * file_name, std::vector<uint8_t> & bytes)
{
  if (!file_name || !file_name[0])
  {
    print_error("Cannot open file. File name is empty.");
    return false;
  }

  if (!is_path_string_valid(file_name))
  {
    print_error("Cannot open file '%s'. Absolute paths or access to the parent directory is prohibited.", file_name);
    return false;
  }

  FILE * f = fopen(file_name, "rb");
  if (!f)
  {
    print_error("Cannot open file '%s'", file_name);
    return false;
  }

  fseek(f, 0, SEEK_END);
  const uint32_t fileLength = uint32_t(ftell(f));
  fseek(f, 0, SEEK_SET);
  bytes.resize(fileLength);
  if (fread((void*)&bytes[0], 1, fileLength, f) != fileLength)
  {
    fclose(f);
    bytes.clear();
    print_error("Cannot read file '%s'", file_name);
    return false;
  }

  fclose(f);
  return true;
}


DasboxFsFileAccess::DasboxFsFileAccess(const char * pak, bool allow_hot_reload) :
  das::ModuleFileAccess(pak, das::make_smart<DasboxFsFileAccess>(false)), storeOpenedFiles(allow_hot_reload)
{
}

DasboxFsFileAccess::DasboxFsFileAccess(bool allow_hot_reload) :
  storeOpenedFiles(allow_hot_reload)
{
}

DasboxFsFileAccess::DasboxFsFileAccess(DasboxFsFileAccess * modAccess, bool allow_hot_reload) :
  storeOpenedFiles(allow_hot_reload)
{
  if (modAccess)
  {
    context = modAccess->context;
    modGet = modAccess->modGet;
    includeGet = modAccess->includeGet;
    moduleAllowed = modAccess->moduleAllowed;
    derivedAccess = true;
  }
}

DasboxFsFileAccess::~DasboxFsFileAccess()
{
  if (derivedAccess)
    context = nullptr;
}

das::FileInfo * DasboxFsFileAccess::getNewFileInfo(const das::string & fname)
{
  FILE * f = fopen(fname.c_str(), "rb");
  if (!f)
  {
    const char * ptr = fname.c_str();
    if (!strncmp(fname.c_str(), "daslib/", 7) || strstr(fname.c_str(), "/daslib/") || strstr(fname.c_str(), "\\daslib/"))
      ptr = std::max(strrchr(ptr, '/'), strrchr(ptr, '\\')) + 1;

    std::string key(ptr);
    auto it = daslib_inc_files.find(key);
    if (it != daslib_inc_files.end())
      return it->second;

    print_error("Script file '%s' not found", fname.c_str());
    return nullptr;
  }

  fseek(f, 0, SEEK_END);
  const uint32_t fileLength = uint32_t(ftell(f));
  fseek(f, 0, SEEK_SET);

  char * source = (char *)das_aligned_alloc16(fileLength + 1);
  if (fread((void*)source, 1, fileLength, f) != fileLength)
  {
    fclose(f);
    free(source);
    print_error("Cannot read file '%s'", fname.c_str());
    return nullptr;
  }

  fclose(f);

  source[fileLength] = 0;
  auto info = das::make_unique<das::TextFileInfo>(source, fileLength, true);
  if (storeOpenedFiles)
  {
    int64_t fileTime = get_file_time(fname.c_str());
    filesOpened.emplace_back(fname, fileTime);
  }
  return setFileInfo(fname, std::move(info));
}

das::ModuleInfo DasboxFsFileAccess::getModuleInfo(const das::string & req, const das::string & from) const
{
  if (!failed())
    return das::ModuleFileAccess::getModuleInfo(req, from);
  bool req_uses_mnt = strncmp(req.c_str(), "%", 1) == 0;
  return das::ModuleFileAccess::getModuleInfo(req, req_uses_mnt ? das::string() : from);
}



inline bool is_slash(char c)
{
  return c == '\\' || c == '/';
}

bool is_path_string_valid(const char * path)
{
  if (trust_mode)
    return true;

  if (!path || !path[0])
    return true;
  if (strchr(path, ':'))
    return false;
  if (path[0] == '/' || (path[0] == '~' && path[1] == '/'))
    return false;

  if (path[0] == '.' && path[1] == '.' && is_slash(path[2]))
    return false;

  int len = int(strlen(path));
  int depth = 0;
  for (int i = 0; i < len; i++)
  {
    if (is_slash(path[i]) && !is_slash(path[i + 1]))
      depth++;
    if (path[i] == '.' && path[i + 1] == '.')
      depth--;
    if (depth < 0)
      return false;
  }

  return true;
}

string combine_path(const string & path1, const string & path2)
{
  string res = path1;
  while (!res.empty() && (res.back() == '/' || res.back() == '\\'))
    res.pop_back();
  res += '/';
  res += path2;
  return res;
}

string extract_dir(const string & path)
{
  const char * p = path.c_str();
  const char * slash = std::max(strrchr(p, '/'), strrchr(p, '\\'));
  return slash ? string(p, slash) : string("");
}

string extract_file_name(const string & path)
{
  const char * p = path.c_str();
  const char * slash = std::max(strrchr(p, '/'), strrchr(p, '\\'));
  return slash ? string(slash + 1) : path;
}

bool change_dir(const string & dir)
{
  if (dir.empty())
    return true;
  string s = dir;
  while (!s.empty() && (s.back() == '/' || s.back() == '\\'))
    s.pop_back();
  return chdir(s.c_str()) == 0;
}

std::string get_current_dir()
{
  char buffer[512] = { 0 };
  return std::string(getcwd(buffer, sizeof(buffer)));
}

bool is_file_exists(const char * file_name)
{
  if (!file_name)
    return false;
  struct stat buffer;
  return stat(file_name, &buffer) == 0;
}

uint64_t get_file_time(const char * file_name)
{
  if (!file_name)
    return 0;
  struct stat buf;
  if (!stat(file_name, &buf))
    return uint64_t(buf.st_mtime);
  return 0;
}


}
