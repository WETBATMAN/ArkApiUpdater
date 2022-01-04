#include "AutoUpdate.h"
#include "Requests.h"
#include "Logger.h"
#include "zip_file.hpp"
#include "sha512.hh"
#include <fstream>
#include <locale>
#include <codecvt>
#include <filesystem>

#define LOGINFO(Data) Log::GetLog()->info(Data)
#define LOGERROR(Data) Log::GetLog()->error(Data)

AutoUpdate& AutoUpdate::Get()
{
	static AutoUpdate instance;
	return instance;
}

void AutoUpdate::OpenConsole()
{
	AllocConsole();
	FILE* p_cout;
	freopen_s(&p_cout, "conout$", "w", stdout);
}

std::string AutoUpdate::GetCurrentDir()
{
	char Buffer[MAX_PATH];
	GetModuleFileNameA(NULL, Buffer, MAX_PATH);

	const std::string::size_type Pos = std::string(Buffer).find_last_of("\\/");

	return std::string(Buffer).substr(0, Pos);
}

bool AutoUpdate::ReadConfig(const std::string& CurrentDir)
{
	try
	{
		std::fstream Config;

		Config.open(CurrentDir + "\\auto_update_config.json", std::fstream::in);

		if (!Config) return false;

		nlohmann::json JsonConf;
		Config >> JsonConf;

		if (JsonConf.is_null()) return false;

		Enabled_ = JsonConf["Enabled"].get<bool>();
		UseBeta_ = JsonConf["UsingBeta"].get<bool>();

		return true;
	}
	catch (const std::exception& e)
	{
		LOGERROR(e.what());
		return false;
	}

	return false;
}

std::string AutoUpdate::GetBranchName()
{
	if (UseBeta_) return "beta";
	else return "master";
}

nlohmann::json AutoUpdate::GetRepoData()
{
	try
	{
		const std::string& res = Requests::Get().CreateGetRequest("https://api.github.com/repos/Michidu/ARK-Server-API/releases", { "user-agent:ArkApi AutoUpdate" });

		if (!res.empty())
		{
			return nlohmann::ordered_json::parse(res);
		}
	}
	catch (const std::exception& e)
	{
		LOGERROR(e.what());
	}

	return nlohmann::json();
}

bool AutoUpdate::ParseRepoData(const nlohmann::ordered_json& Data, const std::string& BranchName, AutoUpdate::RepoData& RepoData)
{
	try
	{
		for (const auto& iter : Data)
		{
			if (iter["target_commitish"].get<std::string>() == BranchName)
			{
				RepoData.ReleaseTag = iter["tag_name"].get<std::string>();
				for (const auto& asset : iter["assets"])
				{
					if (asset.find("browser_download_url") != asset.end())
					{
						RepoData.DownloadURL = asset["browser_download_url"].get<std::string>();
						break;
					}
				}

				if (!RepoData.ReleaseTag.empty() && !RepoData.DownloadURL.empty()) return true;
			}
		}
	}
	catch (const std::exception& e)
	{
		LOGERROR(e.what());
	}

	return false;
}

void AutoUpdate::RemoveOldDll(const std::string& CurrentDir)
{
	remove((CurrentDir + "\\version.dll.old").c_str());
}

bool AutoUpdate::CreateAPIDirs(const std::string& BaseDir)
{
	try
	{
		if (!CreateDirectoryA((BaseDir + "\\ArkApi").c_str(), NULL) && GetLastError() != ERROR_ALREADY_EXISTS) return false;
		if (!CreateDirectoryA((BaseDir + "\\ArkApi\\Plugins").c_str(), NULL) && GetLastError() != ERROR_ALREADY_EXISTS) return false;

		return true;
	}
	catch (const std::exception& e)
	{
		LOGERROR(e.what());
		return false;
	}
}

bool AutoUpdate::CreateAutoUpdateDirs(const std::string& BaseDir)
{
	try
	{
		if (!CreateDirectoryA((BaseDir + "\\ArkApi_AutoUpdate").c_str(), NULL) && GetLastError() != ERROR_ALREADY_EXISTS) return false;
		if (!CreateDirectoryA((BaseDir + "\\ArkApi_AutoUpdate\\Temp").c_str(), NULL) && GetLastError() != ERROR_ALREADY_EXISTS) return false;
		if (!CreateDirectoryA((BaseDir + "\\ArkApi_AutoUpdate\\Version").c_str(), NULL) && GetLastError() != ERROR_ALREADY_EXISTS) return false;

		TempDirPath_ = BaseDir + "\\ArkApi_AutoUpdate\\Temp";
		VersionDirPath_ = BaseDir + "\\ArkApi_AutoUpdate\\Version\\Version.json";

		std::fstream VersionConfig;

		VersionConfig.open(VersionDirPath_, std::fstream::in | std::fstream::out);

		if (!VersionConfig)
		{
			VersionConfig.open(VersionDirPath_, std::fstream::in | std::fstream::out | std::fstream::trunc);
			VersionConfig << "{}";
		}
		else
		{
			nlohmann::json JsonConf;
			VersionConfig >> JsonConf;

			if (!JsonConf.is_null() && JsonConf.find("ReleaseTag") != JsonConf.end())
			{
				if (JsonConf.find("ReleaseTag") != JsonConf.end()) LocalReleaseTag_ = JsonConf["ReleaseTag"].get<std::string>();
				if (JsonConf.find("BranchName") != JsonConf.end()) LocalBranchName_ = JsonConf["BranchName"].get<std::string>();
			}
		}

		VersionConfig.close();

		return true;
	}
	catch (const std::exception& e)
	{
		LOGERROR(e.what());
		return false;
	}
}

bool AutoUpdate::DownloadLatestRelease(const std::string& DownloadURL)
{
	const std::string FilePath = TempDirPath_ + "\\ArkApi.zip";
	return Requests::Get().DownloadFile(DownloadURL, FilePath);
}

void AutoUpdate::MoveUpdateFile(const std::string& FileName, const std::string& CurrentDir)
{
	if (FileName == "version.dll")
	{
		const std::string BaseStr = CurrentDir + "\\";
		rename((BaseStr + "version.dll").c_str(), (BaseStr + "version.dll.old").c_str());
		RebootRequired_ = true;
	}

	const std::string ExtractedFile = TempDirPath_ + "\\" + FileName;
	const std::string OldFile = CurrentDir + "\\" + FileName;

	remove(OldFile.c_str());
	const auto res = rename(ExtractedFile.c_str(), OldFile.c_str());

	if (res == 0)
	{
		LOGINFO("Sucesfully updated: " + FileName);
		UpdatedFiles_++;
	}
	else LOGERROR("Error updating: " + FileName + " Error code: " + std::to_string(res));
}

bool AutoUpdate::ShouldUpdate(const std::string& FileName, const std::string& CurrentDir)
{
	const std::string& CurrentFileHash = sw::sha512::file(CurrentDir + "\\" + FileName);
	const std::string& NewFileHash = sw::sha512::file(TempDirPath_ + "\\" + FileName);

	return CurrentFileHash != NewFileHash;
}

bool AutoUpdate::UpdateFiles(const std::string& CurrentDir)
{
	try
	{
		miniz_cpp::zip_file File(TempDirPath_ + "\\ArkApi.zip");
		const std::string UpdateFiles[] = { "config.json", "msdia140.dll", "version.dll", "version.pdb" };

		for (const auto& UpdateFile : UpdateFiles)
		{
			if (UpdateFile == "config.json")
			{
				std::fstream ConfigFile;
				ConfigFile.open(CurrentDir + "\\" + "config.json", std::fstream::in | std::fstream::out);

				if (!ConfigFile && File.has_file(UpdateFile))
				{
					File.extract(UpdateFile, TempDirPath_);
					MoveUpdateFile(UpdateFile, CurrentDir);
				}
				else ConfigFile.close();
			}
			else
			{
				File.extract(UpdateFile, TempDirPath_);

				if (ShouldUpdate(UpdateFile, CurrentDir))
				{
					MoveUpdateFile(UpdateFile, CurrentDir);
				}
			}
		}
	}
	catch (const std::exception& e)
	{
		LOGERROR(e.what());
		return false;
	}

	return true;
}

bool AutoUpdate::WriteNewManifest(const std::string& ReleaseTag, const std::string& BranchName)
{
	try
	{
		nlohmann::json Manifest;
		Manifest["ReleaseTag"] = ReleaseTag;
		Manifest["BranchName"] = BranchName;

		std::fstream VersionConfig;
		VersionConfig.open(VersionDirPath_, std::fstream::out | std::fstream::trunc);

		if (VersionConfig)
		{
			VersionConfig << Manifest.dump();
			VersionConfig.close();
		}
		else return false;
	}
	catch (const std::exception& e)
	{
		LOGERROR(e.what());
		return false;
	}

	return true;
}

void AutoUpdate::DeleteTempFiles()
{
	for (const auto& File : std::filesystem::directory_iterator(TempDirPath_))
	{
		std::filesystem::remove_all(File.path());
	}
}

void AutoUpdate::RelaunchServer(const std::string& CurrentDir)
{
	LOGINFO("This update requires a server reboot, rebooting in 5 seconds...");

	Sleep(5000);

	const std::wstring WCurrentDir(CurrentDir.begin(), CurrentDir.end());

	STARTUPINFOW SI;
	PROCESS_INFORMATION PI;

	ZeroMemory(&SI, sizeof(SI));
	SI.cb = sizeof(SI);
	ZeroMemory(&PI, sizeof(PI));

	if (!CreateProcessW((WCurrentDir + LR"(\ShooterGameServer.exe)").c_str(), GetCommandLineW(), nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE, nullptr, nullptr, &SI, &PI))
	{
		LOGERROR("Failed to Create New Process");
		return;
	}

	Sleep(1000);

	CloseHandle(PI.hProcess);
	CloseHandle(PI.hThread);

	exit(EXIT_SUCCESS);
}

void AutoUpdate::Run(HMODULE hModule)
{
	OpenConsole();

	LOGINFO("Starting automatic update process");

	const std::string& CurrentDir = GetCurrentDir();

	if (CurrentDir.empty())
	{
		LOGERROR("Failed to read directory");
		return;
	}

	if (!ReadConfig(CurrentDir))
	{
		LOGERROR("Failed to read config");
		return;
	}

	if (!Enabled_)
	{
		LOGINFO("AutoUpdate is disabled, skipping update");
		return;
	}

	if (UseBeta_)
	{
		LOGINFO("Beta ArkApi downloads are enabled");
	}

	const nlohmann::json& RepoData = GetRepoData();

	const std::string& BranchName = GetBranchName();

	AutoUpdate::RepoData ParsedData;
	if (!ParseRepoData(RepoData, BranchName, ParsedData))
	{
		LOGERROR("Could not parse repo data");
		return;
	}

	const std::string& DownloadURL = ParsedData.DownloadURL;
	const std::string& ReleaseTag = ParsedData.ReleaseTag;

	RemoveOldDll(CurrentDir);

	if (!CreateAPIDirs(CurrentDir))
	{
		LOGERROR("Failed to create API directories");
		return;
	}

	if (!CreateAutoUpdateDirs(CurrentDir))
	{
		LOGERROR("Failed to create AutoUpdate directories");
		return;
	}

	if (LocalReleaseTag_ == ReleaseTag && LocalBranchName_ == BranchName)
	{
		LOGINFO("Latest ArkApi version is installed, skipping update");
		return;
	}

	if (!DownloadLatestRelease(DownloadURL))
	{
		LOGERROR("Failed to download release files");
		return;
	}

	if (!UpdateFiles(CurrentDir))
	{
		LOGERROR("Failed to unpack release files");
		return;
	}

	if (!WriteNewManifest(ReleaseTag, BranchName))
	{
		LOGERROR("Failed to write new update version");
		return;
	}

	DeleteTempFiles();

	if (UpdatedFiles_ > 0) LOGINFO("Update complete, " + std::to_string(UpdatedFiles_) + " x File(s) required an update");
	else LOGINFO("Update complete, no files required updates");

	if (RebootRequired_) RelaunchServer(CurrentDir);
}