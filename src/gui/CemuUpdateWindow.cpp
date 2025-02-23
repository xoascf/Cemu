#include "gui/CemuUpdateWindow.h"

#include "Common/version.h"
#include "util/helpers/helpers.h"
#include "util/helpers/SystemException.h"
#include "config/ActiveSettings.h"
#include "Common/filestream.h"

#include <wx/sizer.h>
#include <wx/gauge.h>
#include <wx/button.h>
#include <wx/msgdlg.h>

#include <curl/curl.h>
#include <zip.h>
#include <boost/tokenizer.hpp>


wxDECLARE_EVENT(wxEVT_RESULT, wxCommandEvent);
wxDEFINE_EVENT(wxEVT_RESULT, wxCommandEvent);

wxDECLARE_EVENT(wxEVT_PROGRESS, wxCommandEvent);
wxDEFINE_EVENT(wxEVT_PROGRESS, wxCommandEvent);

CemuUpdateWindow::CemuUpdateWindow(wxWindow* parent)
	: wxDialog(parent, wxID_ANY, "Cemu update", wxDefaultPosition, wxDefaultSize,
	           wxCAPTION | wxMINIMIZE_BOX | wxSYSTEM_MENU | wxTAB_TRAVERSAL | wxCLOSE_BOX)
{
	auto* sizer = new wxBoxSizer(wxVERTICAL);
	m_gauge = new wxGauge(this, wxID_ANY, 100, wxDefaultPosition, wxSize(500, 20), wxGA_HORIZONTAL);
	m_gauge->SetValue(0);
	sizer->Add(m_gauge, 0, wxALL | wxEXPAND, 5);

	auto* rows = new wxFlexGridSizer(0, 2, 0, 0);
	rows->AddGrowableCol(1);

	m_text = new wxStaticText(this, wxID_ANY, "Checking for latest version...");
	rows->Add(m_text, 0, wxALL | wxALIGN_CENTER_VERTICAL, 5);

	{
		auto* right_side = new wxBoxSizer(wxHORIZONTAL);

		m_update_button = new wxButton(this, wxID_ANY, _("Update"));
		m_update_button->Bind(wxEVT_BUTTON, &CemuUpdateWindow::OnUpdateButton, this);
		right_side->Add(m_update_button, 0, wxALL, 5);

		m_cancel_button = new wxButton(this, wxID_ANY, _("Cancel"));
		m_cancel_button->Bind(wxEVT_BUTTON, &CemuUpdateWindow::OnCancelButton, this);
		right_side->Add(m_cancel_button, 0, wxALL, 5);

		rows->Add(right_side, 1, wxALIGN_RIGHT, 5);
	}

	m_changelog = new wxHyperlinkCtrl(this, wxID_ANY, _("Changelog"), wxEmptyString);
	rows->Add(m_changelog, 0, wxLEFT | wxBOTTOM | wxRIGHT | wxEXPAND, 5);

	sizer->Add(rows, 0, wxALL | wxEXPAND, 5);

	SetSizerAndFit(sizer);
	Centre(wxBOTH);

	Bind(wxEVT_CLOSE_WINDOW, &CemuUpdateWindow::OnClose, this);
	Bind(wxEVT_RESULT, &CemuUpdateWindow::OnResult, this);
	Bind(wxEVT_PROGRESS, &CemuUpdateWindow::OnGaugeUpdate, this);
	m_thread = std::thread(&CemuUpdateWindow::WorkerThread, this);

	m_update_button->Hide();
	m_changelog->Hide();
}

CemuUpdateWindow::~CemuUpdateWindow()
{
	m_order = WorkerOrder::Exit;
	if (m_thread.joinable())
		m_thread.join();
}

size_t CemuUpdateWindow::WriteStringCallback(char* ptr, size_t size, size_t nmemb, void* userdata)
{
	((std::string*)userdata)->append(ptr, size * nmemb);
	return size * nmemb;
};

std::string _curlUrlEscape(CURL* curl, const std::string& input)
{
	char* escapedStr = curl_easy_escape(curl, input.c_str(), input.size());
	std::string r(escapedStr);
	curl_free(escapedStr);
	return r;
}

bool CemuUpdateWindow::GetServerVersion(uint64& version, std::string& filename, std::string& changelog_filename)
{
	std::string buffer;
	std::string urlStr("https://cemu.info/api/cemu_version3.php?version2=");
	auto* curl = curl_easy_init();
	urlStr.append(_curlUrlEscape(curl, fmt::format("{}.{}.{}{}", EMULATOR_VERSION_LEAD, EMULATOR_VERSION_MAJOR, EMULATOR_VERSION_MINOR, EMULATOR_VERSION_SUFFIX)));

	curl_easy_setopt(curl, CURLOPT_URL, urlStr.c_str());
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteStringCallback);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

	bool result = false;
	CURLcode cr = curl_easy_perform(curl);
	if (cr == CURLE_OK)
	{
		long http_code = 0;
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
		if (http_code != 0 && http_code != 200)
		{
			forceLog_printf("Update check failed (http code: %d)", http_code);
			cemu_assert_debug(false);
			return false;
		}
		
		std::vector<std::string> tokens;
		const boost::char_separator<char> sep{ "|" };
		for (const auto& token : boost::tokenizer(buffer, sep))
		{
			tokens.emplace_back(token);
		}

		if (tokens.size() >= 2)
		{
			const auto latest_version = ConvertString<uint64>(tokens[0]);
			result = latest_version > 0 && !tokens[1].empty();
			if (result)
			{
				version = latest_version;
				filename = tokens[1];
				
				if(tokens.size() >= 3)
					changelog_filename = tokens[2];
			}
		}
	}
	else
	{
		forceLog_printf("Update check failed with CURL error %d", (int)cr);
		cemu_assert_debug(false);
	}

	curl_easy_cleanup(curl);
	return result;
}

std::future<bool> CemuUpdateWindow::IsUpdateAvailable()
{
	return std::async(std::launch::async, CheckVersion);
}

bool CemuUpdateWindow::CheckVersion()
{
	uint64 latest_version;
	std::string filename, changelog;
	if (!GetServerVersion(latest_version, filename, changelog))
		return false;

	return IsUpdateAvailable(latest_version);
}

bool CemuUpdateWindow::IsUpdateAvailable(uint64 latest_version)
{
	uint64 version = EMULATOR_SERVER_VERSION;

	return latest_version > version;
}

int CemuUpdateWindow::ProgressCallback(void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal,
                                       curl_off_t ulnow)
{
	auto* thisptr = (CemuUpdateWindow*)clientp;
	auto* event = new wxCommandEvent(wxEVT_PROGRESS);
	event->SetInt((int)dlnow);
	wxQueueEvent(thisptr, event);
	return 0;
}

bool CemuUpdateWindow::DownloadCemuZip(const std::string& url, const fs::path& filename)
{
	FileStream* fsUpdateFile = FileStream::createFile2(filename);
	if (!fsUpdateFile)
		return false;

	bool result = false;
	auto* curl = curl_easy_init();
	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl, CURLOPT_NOBODY, 1);
	if (curl_easy_perform(curl) == CURLE_OK)
	{
		long http_code = 0;
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
		if (http_code != 0 && http_code != 200)
		{
			cemuLog_log(LogType::Force, "Unable to download cemu update zip file from {} (http error: {})", url, http_code);
			curl_easy_cleanup(curl);
			return false;
		}
		
		curl_off_t update_size;
		if (curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &update_size) == CURLE_OK)
			m_gauge_max_value = (int)update_size;


		auto _curlWriteData = +[](void* ptr, size_t size, size_t nmemb, void* ctx) -> size_t
		{
			FileStream* fs = (FileStream*)ctx;
			const size_t writeSize = size * nmemb;
			fs->writeData(ptr, writeSize);
			return writeSize;
		};

		curl_easy_setopt(curl, CURLOPT_NOBODY, 0);
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, _curlWriteData);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, fsUpdateFile);
		curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0);
		curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, ProgressCallback);
		curl_easy_setopt(curl, CURLOPT_XFERINFODATA, this);

		auto curl_result = std::async(std::launch::async, [](CURL* curl, long* http_code)
		{
			const auto r = curl_easy_perform(curl);
			curl_easy_cleanup(curl);
			return r;
		}, curl, &http_code);
		while (!curl_result.valid())
		{
			if (m_order == WorkerOrder::Exit)
				return false;

			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		
		result = curl_result.get() == CURLE_OK;

		delete fsUpdateFile;
	}
	else
		curl_easy_cleanup(curl);

	if (!result && fs::exists(filename))
	{
		try
		{
			fs::remove(filename);
		}
		catch (const std::exception& ex)
		{
			forceLog_printf("can't remove update.zip on error: %s", ex.what());
		}
	}
	return result;
}

bool CemuUpdateWindow::ExtractUpdate(const fs::path& zipname, const fs::path& targetpath)
{
	// open downloaded zip
	int err;
	auto* za = zip_open(zipname.string().c_str(), ZIP_RDONLY, &err);
	if (za == nullptr)
	{
		cemuLog_log(LogType::Force, "Cannot open zip file: {}", zipname.string());
		return false;
	}

	const auto count = zip_get_num_entries(za, 0);
	m_gauge_max_value = count;
	for (auto i = 0; i < count; i++)
	{
		if (m_order == WorkerOrder::Exit)
			return false;

		zip_stat_t sb{};
		if (zip_stat_index(za, i, 0, &sb) == 0)
		{
			fs::path fname = targetpath;
			fname /= sb.name;

			const auto len = strlen(sb.name);
			if (strcmp(sb.name, ".") == 0 || strcmp(sb.name, "..") == 0)
			{
				// protection
				continue;
			}
			if (sb.name[len - 1] == '/' || sb.name[len - 1] == '\\')
			{
				// directory
				try
				{
					if (!exists(fname))
						create_directory(fname);
				}
				catch (const std::exception& ex)
				{
					SystemException sys(ex);
					forceLog_printf("can't create folder \"%s\" for update: %s", sb.name, sys.what());
				}
				continue;
			}

			// file
			auto* zf = zip_fopen_index(za, i, 0);
			if (!zf)
			{
				forceLog_printf("can't open zip file \"%s\"", sb.name);
				zip_close(za);
				return false;
			}

			std::vector<char> buffer(sb.size);
			const auto read = zip_fread(zf, buffer.data(), sb.size);
			if (read != (sint64)sb.size)
			{
				forceLog_printf("could only read 0x%x of 0x%x bytes from zip file \"%s\"", read, sb.size, sb.name);
				zip_close(za);
				return false;
			}

			auto* file = fopen(fname.string().c_str(), "wb");
			if (file == nullptr)
			{
				forceLog_printf("can't create update file \"%s\"", sb.name);
				zip_close(za);
				return false;
			}

			fwrite(buffer.data(), 1, buffer.size(), file);
			fflush(file);
			fclose(file);

			zip_fclose(zf);

			if ((i / 10) * 10 == i)
			{
				auto* event = new wxCommandEvent(wxEVT_PROGRESS);
				event->SetInt(i);
				wxQueueEvent(this, event);
			}
		}
	}

	auto* event = new wxCommandEvent(wxEVT_PROGRESS);
	event->SetInt(m_gauge_max_value);
	wxQueueEvent(this, event);

	zip_close(za);

	return true;
}

void CemuUpdateWindow::WorkerThread()
{
	const auto tmppath = fs::temp_directory_path() / L"cemu_update";
	std::error_code ec;
	// clean leftovers
	if (exists(tmppath))
		remove_all(tmppath, ec);

	while (true)
	{
		std::unique_lock lock(m_mutex);
		while (m_order == WorkerOrder::Idle)
			m_condition.wait_for(lock, std::chrono::milliseconds(125));

		if (m_order == WorkerOrder::Exit)
			break;

		try
		{
			if (m_order == WorkerOrder::CheckVersion)
			{
				auto* event = new wxCommandEvent(wxEVT_RESULT);
				if (GetServerVersion(m_version, m_filename, m_changelog_filename) && IsUpdateAvailable(m_version))
					event->SetInt((int)Result::UpdateAvailable);
				else
					event->SetInt((int)Result::NoUpdateAvailable);

				wxQueueEvent(this, event);
			}
			else if (m_order == WorkerOrder::UpdateVersion)
			{
				// download update
				const std::string url = fmt::format("http://cemu.info/releases/{}", m_filename);
				if (!exists(tmppath))
					create_directory(tmppath);

				const auto update_file = tmppath / L"update.zip";
				if (DownloadCemuZip(url, update_file))
				{
					auto* event = new wxCommandEvent(wxEVT_RESULT);
					event->SetInt((int)Result::UpdateDownloaded);
					wxQueueEvent(this, event);
				}
				else
				{
					auto* event = new wxCommandEvent(wxEVT_RESULT);
					event->SetInt((int)Result::UpdateDownloadError);
					wxQueueEvent(this, event);
					m_order = WorkerOrder::Idle;
					continue;
				}
				if (m_order == WorkerOrder::Exit)
					break;

				// extract
				const auto expected_path = (tmppath / m_filename).replace_extension("");
				if (ExtractUpdate(update_file, tmppath) && exists(expected_path))
				{
					auto* event = new wxCommandEvent(wxEVT_RESULT);
					event->SetInt((int)Result::ExtractSuccess);
					wxQueueEvent(this, event);
				}
				else
				{
					auto* event = new wxCommandEvent(wxEVT_RESULT);
					event->SetInt((int)Result::ExtractError);
					wxQueueEvent(this, event);

					if (exists(tmppath))
					{
						try
						{
							fs::remove(tmppath);
						}
						catch (const std::exception& ex)
						{
							SystemException sys(ex);
							forceLog_printf("can't remove extracted tmp files: %s", sys.what());
						}
					}

					continue;
				}

				if (m_order == WorkerOrder::Exit)
					break;

				// apply update
				std::wstring target_directory = ActiveSettings::GetPath().generic_wstring();
				if (target_directory[target_directory.size() - 1] == '/')
					target_directory = target_directory.substr(0, target_directory.size() - 1); // remove trailing /

				// get exe name
				const auto exec = ActiveSettings::GetFullPath();
				const auto target_exe = fs::path(exec).replace_extension("exe.backup");
				fs::rename(exec, target_exe);
				m_restart_file = exec;

				const auto index = expected_path.wstring().size();
				int counter = 0;
				for (const auto& it : fs::recursive_directory_iterator(expected_path))
				{
					const auto filename = it.path().wstring().substr(index);
					auto target_file = target_directory + filename;
					try
					{
						if (is_directory(it))
						{
							if (!fs::exists(target_file))
								fs::create_directory(target_file);
						}
						else
						{
							if(it.path().filename() == L"Cemu.exe")
								fs::rename(it.path(), fs::path(target_file).replace_filename(exec.filename()));
							else
								fs::rename(it.path(), target_file);
						}
					}
					catch (const std::exception& ex)
					{
						SystemException sys(ex);
						forceLog_printf("applying update error: %s", sys.what());
					}

					if ((counter++ / 10) * 10 == counter)
					{
						auto* event = new wxCommandEvent(wxEVT_PROGRESS);
						event->SetInt(counter);
						wxQueueEvent(this, event);
					}
				}

				auto* event = new wxCommandEvent(wxEVT_PROGRESS);
				event->SetInt(m_gauge_max_value);
				wxQueueEvent(this, event);

				auto* result_event = new wxCommandEvent(wxEVT_RESULT);
				result_event->SetInt((int)Result::Success);
				wxQueueEvent(this, result_event);
			}
		}
		catch (const std::exception& ex)
		{
			SystemException sys(ex);
			forceLog_printf("update error: %s", sys.what());

			// clean leftovers
			if (exists(tmppath))
				remove_all(tmppath, ec);

			auto* result_event = new wxCommandEvent(wxEVT_RESULT);
			result_event->SetInt((int)Result::Error);
			wxQueueEvent(this, result_event);
		}

		m_order = WorkerOrder::Idle;
	}
}

void CemuUpdateWindow::OnClose(wxCloseEvent& event)
{
	event.Skip();
	
#if BOOST_OS_WINDOWS
	if (m_restart_required && !m_restart_file.empty() && fs::exists(m_restart_file))
	{
		PROCESS_INFORMATION pi{};
		STARTUPINFO si{};
		si.cb = sizeof(si);

		std::wstring cmdline = GetCommandLineW();
		const auto index = cmdline.find('"', 1);
		cemu_assert_debug(index != std::wstring::npos);
		cmdline = L"\"" + m_restart_file.wstring() + L"\"" + cmdline.substr(index + 1);

		HANDLE lock = CreateMutex(nullptr, TRUE, L"Global\\cemu_update_lock");
		CreateProcess(nullptr, (wchar_t*)cmdline.c_str(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi);
		
		exit(0);
	}
#else
	cemuLog_log(LogType::Force, "unimplemented - restart on update");
#endif
}


void CemuUpdateWindow::OnResult(wxCommandEvent& event)
{
	switch ((Result)event.GetInt())
	{
	case Result::NoUpdateAvailable:
		m_cancel_button->SetLabel(_("Exit"));
		m_text->SetLabel(_("No update available!"));
		m_gauge->SetValue(100);
		break;
	case Result::UpdateAvailable:
		{
			if (!m_changelog_filename.empty())
			{
				m_changelog->SetURL(fmt::format("https://cemu.info/changelog/{}", m_changelog_filename));
				m_changelog->Show();
			}
			else
				m_changelog->Hide();
			
			m_update_button->Show();
			

			m_text->SetLabel(_("Update available!"));
			m_cancel_button->SetLabel(_("Exit"));
			break;
		}
	case Result::UpdateDownloaded:
		m_text->SetLabel(_("Extracting update..."));
		m_gauge->SetValue(0);
		break;
	case Result::UpdateDownloadError:
		m_update_button->Enable();
		m_text->SetLabel(_("Couldn't download the update!"));
		break;
	case Result::ExtractSuccess:
		m_text->SetLabel(_("Applying update..."));
		m_gauge->SetValue(0);
		m_cancel_button->Disable();
		break;
	case Result::ExtractError:
		m_update_button->Enable();
		m_cancel_button->Enable();
		m_text->SetLabel(_("Extracting failed!"));
		break;
	case Result::Success:
		m_cancel_button->Enable();
		m_update_button->Hide();

		m_text->SetLabel(_("Success"));
		m_cancel_button->SetLabel(_("Restart"));
		m_restart_required = true;
		break;
	default: ;
	}
}

void CemuUpdateWindow::OnGaugeUpdate(wxCommandEvent& event)
{
	const int total_size = m_gauge_max_value > 0 ? m_gauge_max_value : 10000000;
	m_gauge->SetValue((event.GetInt() * 100) / total_size);
}

void CemuUpdateWindow::OnUpdateButton(const wxCommandEvent& event)
{
	std::unique_lock lock(m_mutex);
	m_order = WorkerOrder::UpdateVersion;

	m_condition.notify_all();

	m_update_button->Disable();

	m_text->SetLabel(_("Downloading update..."));
}

void CemuUpdateWindow::OnCancelButton(const wxCommandEvent& event)
{
	Close();
}
