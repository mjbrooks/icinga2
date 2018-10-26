/* Icinga 2 | (c) 2012 Icinga GmbH | GPLv2+ */

#include "remote/apilistener.hpp"
#include "remote/apifunction.hpp"
#include "config/configcompiler.hpp"
#include "base/tlsutility.hpp"
#include "base/json.hpp"
#include "base/configtype.hpp"
#include "base/logger.hpp"
#include "base/convert.hpp"
#include "base/exception.hpp"
#include "base/utility.hpp"
#include <fstream>
#include <iomanip>

using namespace icinga;

REGISTER_APIFUNCTION(Update, config, &ApiListener::ConfigUpdateHandler);

boost::mutex ApiListener::m_ConfigSyncStageLock;

/**
 * Entrypoint for updating all authoritative configs into var/lib/icinga2/api/zones
 *
 */
void ApiListener::SyncLocalZoneDirs() const
{
	for (const Zone::Ptr& zone : ConfigType::GetObjectsByType<Zone>()) {
		try {
			SyncLocalZoneDir(zone);
		} catch (const std::exception&) {
			continue;
		}
	}
}

/**
 * Sync a zone directory where we have an authoritative copy (zones.d, etc.)
 *
 * This function collects the registered zone config dirs from
 * the config compiler and reads the file content into the config
 * information structure.
 *
 * Returns early when there are no updates.
 *
 * @param zone Pointer to the zone object being synced.
 */
void ApiListener::SyncLocalZoneDir(const Zone::Ptr& zone) const
{
	if (!zone)
		return;

	ConfigDirInformation newConfigInfo;
	newConfigInfo.UpdateV1 = new Dictionary();
	newConfigInfo.UpdateV2 = new Dictionary();
	newConfigInfo.Checksums = new Dictionary();

	String zoneName = zone->GetName();

	/* Load registered zone paths, e.g. '_etc', '_api' and user packages. */
	for (const ZoneFragment& zf : ConfigCompiler::GetZoneDirs(zoneName)) {
		ConfigDirInformation newConfigPart = LoadConfigDir(zf.Path);

		/* Config files '*.conf'. */
		{
			ObjectLock olock(newConfigPart.UpdateV1);
			for (const Dictionary::Pair& kv : newConfigPart.UpdateV1) {
				String path = "/" + zf.Tag + kv.first;
				newConfigInfo.UpdateV1->Set(path, kv.second);
				newConfigInfo.Checksums->Set(path, GetChecksum(kv.second));
			}
		}

		/* Meta files. */
		{
			ObjectLock olock(newConfigPart.UpdateV2);
			for (const Dictionary::Pair& kv : newConfigPart.UpdateV2) {
				String path = "/" + zf.Tag + kv.first;
				newConfigInfo.UpdateV2->Set(path, kv.second);
				newConfigInfo.Checksums->Set(path, GetChecksum(kv.second));
			}
		}
	}

	int sumUpdates = newConfigInfo.UpdateV1->GetLength() + newConfigInfo.UpdateV2->GetLength();

	if (sumUpdates == 0)
		return;

	String productionZonesDir = GetApiZonesDir() + zoneName;

	Log(LogInformation, "ApiListener")
		<< "Copying " << sumUpdates << " zone configuration files for zone '" << zoneName << "' to '" << productionZonesDir << "'.";

	/* Purge files to allow deletion via zones.d. */
	if (Utility::PathExists(productionZonesDir))
		Utility::RemoveDirRecursive(productionZonesDir);

	Utility::MkDirP(productionZonesDir, 0700);

	/* Copy content and add additional meta data. */
	size_t numBytes = 0;

	/* Note: We cannot simply copy directories here.
	 *
	 * Zone directories are registered from everywhere and we already
	 * have read their content into memory with LoadConfigDir().
	 */
	Dictionary::Ptr newConfig = MergeConfigUpdate(newConfigInfo);

	{
		ObjectLock olock(newConfig);
		for (const Dictionary::Pair& kv : newConfig) {
			String dst = productionZonesDir + "/" + kv.first;
			Utility::MkDirP(Utility::DirName(dst), 0755);

			Log(LogInformation, "ApiListener")
				<< "Updating configuration file: " << dst;

			String content = kv.second;
			std::ofstream fp(dst.CStr(), std::ofstream::out | std::ostream::binary | std::ostream::trunc);
			fp << content;
			fp.close();

			numBytes += content.GetLength();
		}
	}

	/* Additional metadata. */
	String tsPath = productionZonesDir + "/.timestamp";

	if (!Utility::PathExists(tsPath)) {
		std::ofstream fp(tsPath.CStr(), std::ofstream::out | std::ostream::trunc);
		fp << std::fixed << Utility::GetTime();
		fp.close();
	}

	String authPath = productionZonesDir + "/.authoritative";

	if (!Utility::PathExists(authPath)) {
		std::ofstream fp(authPath.CStr(), std::ofstream::out | std::ostream::trunc);
	}

	String checksumsPath = productionZonesDir + "/.checksums";

	if (Utility::PathExists(checksumsPath))
		(void) unlink(checksumsPath.CStr());

	std::ofstream fp(checksumsPath.CStr(), std::ofstream::out | std::ostream::trunc);
	fp << std::fixed << JsonEncode(newConfigInfo.Checksums);
	fp.close();
}

/**
 * Entrypoint for sending a file based config update to a cluster client.
 * This includes security checks for zone relations.
 * Loads the zone config files where this client belongs to
 * and sends the 'config::Update' JSON-RPC message.
 *
 * @param aclient Connected JSON-RPC client.
 */
void ApiListener::SendConfigUpdate(const JsonRpcConnection::Ptr& aclient)
{
	Endpoint::Ptr endpoint = aclient->GetEndpoint();
	ASSERT(endpoint);

	Zone::Ptr clientZone = endpoint->GetZone();
	Zone::Ptr localZone = Zone::GetLocalZone();

	/* don't try to send config updates to our master */
	if (!clientZone->IsChildOf(localZone))
		return;

	Dictionary::Ptr configUpdateV1 = new Dictionary();
	Dictionary::Ptr configUpdateV2 = new Dictionary();
	Dictionary::Ptr configUpdateChecksums = new Dictionary();

	String zonesDir = GetApiZonesDir();

	for (const Zone::Ptr& zone : ConfigType::GetObjectsByType<Zone>()) {
		String zoneName = zone->GetName();
		String zoneDir = zonesDir + zoneName;

		/* Only sync child and global zones. */
		if (!zone->IsChildOf(clientZone) && !zone->IsGlobal())
			continue;

		/* Zone was configured, but there's no configuration directory. */
		if (!Utility::PathExists(zoneDir))
			continue;

		Log(LogInformation, "ApiListener")
			<< "Syncing configuration files for " << (zone->IsGlobal() ? "global " : "")
			<< "zone '" << zoneName << "' to endpoint '" << endpoint->GetName() << "'.";

		ConfigDirInformation config = LoadConfigDir(zoneDir);

		configUpdateV1->Set(zoneName, config.UpdateV1);
		configUpdateV2->Set(zoneName, config.UpdateV2);
		configUpdateChecksums->Set(zoneName, config.Checksums);
	}

	Dictionary::Ptr message = new Dictionary({
		{ "jsonrpc", "2.0" },
		{ "method", "config::Update" },
		{ "params", new Dictionary({
			{ "update", configUpdateV1 },
			{ "update_v2", configUpdateV2 },	/* Since 2.4.2. */
			{ "checksums", configUpdateChecksums } 	/* Since 2.11.0. */
		}) }
	});

	aclient->SendMessage(message);
}

/**
 * Registered handler when a new config::Update message is received.
 *
 * Checks destination and permissions first, then analyses the update.
 * The newly received configuration is not copied to production immediately,
 * but into the staging directory first.
 * Last, the async validation and restart is triggered.
 *
 * @param origin Where this message came from.
 * @param params Message parameters including the config updates.
 * @returns Empty, required by the interface.
 */
Value ApiListener::ConfigUpdateHandler(const MessageOrigin::Ptr& origin, const Dictionary::Ptr& params)
{
	/* Verify permissions and trust relationship. */
	if (!origin->FromClient->GetEndpoint() || (origin->FromZone && !Zone::GetLocalZone()->IsChildOf(origin->FromZone)))
		return Empty;

	ApiListener::Ptr listener = ApiListener::GetInstance();

	if (!listener) {
		Log(LogCritical, "ApiListener", "No instance available.");
		return Empty;
	}

	if (!listener->GetAcceptConfig()) {
		Log(LogWarning, "ApiListener")
			<< "Ignoring config update. '" << listener->GetName() << "' does not accept config.";
		return Empty;
	}

	/* Only one transaction is allowed, concurrent message handlers need to wait.
	 * This affects two parent endpoints sending the config in the same moment.
	 */
	boost::mutex::scoped_lock lock(m_ConfigSyncStageLock);

	String apiZonesStageDir = GetApiZonesStageDir();
	String fromEndpointName = origin->FromClient->GetEndpoint()->GetName();
	String fromZoneName = GetFromZoneName(origin->FromZone);

	Log(LogInformation, "ApiListener")
		<< "Applying config update from endpoint '" << fromEndpointName
		<< "' of zone '" << fromZoneName << "'.";

	Dictionary::Ptr updateV1 = params->Get("update");
	Dictionary::Ptr updateV2 = params->Get("update_v2");

	/* New since 2.11.0. */
	Dictionary::Ptr checksums;

	if (params->Contains("checksums"))
		checksums = params->Get("checksums");

	bool configChange = false;
	std::vector<String> relativePaths;

	/*
	 * We can and must safely purge the staging directory, as the difference is taken between
	 * runtime production config and newly received configuration.
	 */
	if (Utility::PathExists(apiZonesStageDir))
		Utility::RemoveDirRecursive(apiZonesStageDir);

	Utility::MkDirP(apiZonesStageDir, 0700);

	/* Analyse and process the update. */
	ObjectLock olock(updateV1);
	for (const Dictionary::Pair& kv : updateV1) {

		/* Check for the configured zones. */
		String zoneName = kv.first;
		Zone::Ptr zone = Zone::GetByName(zoneName);

		if (!zone) {
			Log(LogWarning, "ApiListener")
				<< "Ignoring config update from endpoint '" << fromEndpointName
				<< "' for unknown zone '" << zoneName << "'.";
			continue;
		}

		/* Whether we already have configuration in zones.d. */
		if (ConfigCompiler::HasZoneConfigAuthority(zoneName)) {
			Log(LogInformation, "ApiListener")
				<< "Ignoring config update from endpoint '" << fromEndpointName
				<< "' for zone '" << zoneName << "' because we have an authoritative version of the zone's config.";
			continue;
		}

		/* Put the received configuration into our stage directory. */
		String productionConfigZoneDir = GetApiZonesDir() + zoneName;
		String stageConfigZoneDir = GetApiZonesStageDir() + zoneName;

		Utility::MkDirP(productionConfigZoneDir, 0700);
		Utility::MkDirP(stageConfigZoneDir, 0700);

		/* Merge the config information. */
		ConfigDirInformation newConfigInfo;
		newConfigInfo.UpdateV1 = kv.second;

		/* Load metadata. */
		if (updateV2)
			newConfigInfo.UpdateV2 = updateV2->Get(kv.first);

		/* Load checksums. */
		if (checksums)
			newConfigInfo.Checksums = checksums->Get(kv.first);

		/* Load the current production config details. */
		ConfigDirInformation productionConfigInfo = LoadConfigDir(productionConfigZoneDir);

		Dictionary::Ptr productionConfig = MergeConfigUpdate(productionConfigInfo);
		Dictionary::Ptr newConfig = MergeConfigUpdate(newConfigInfo);

		/* If we have received 'checksums' via cluster message, go for it.
		 * Otherwise do the old timestamp dance.
		 */
		if (checksums) {
			/* Calculate and compare the checksums. */
			String productionConfigChecksum = GetGlobalChecksum(productionConfigInfo);
			String newConfigChecksum = GetGlobalChecksum(newConfigInfo);

			Log(LogWarning, "ApiListener")
				<< "Received configuration for zone '" << zoneName << "' from endpoint '"
				<< fromEndpointName << "' with checksum '" << newConfigChecksum << "'."
				<< "Our production configuration has checksum '" << productionConfigChecksum << "'.";

			/* TODO: Do this earlier in hello-handshakes. */
			if (newConfigChecksum != productionConfigChecksum)
				configChange = true;
		} else {
			/* TODO: Figure out whether we always need to rely on the timestamp flags. */
			double productionTimestamp;

			if (!productionConfig->Contains("/.timestamp"))
				productionTimestamp = 0;
			else
				productionTimestamp = productionConfig->Get("/.timestamp");

			double newTimestamp;

			if (!newConfig->Contains("/.timestamp"))
				newTimestamp = Utility::GetTime();
			else
				newTimestamp = newConfig->Get("/.timestamp");

			/* skip update if our configuration files are more recent */
			if (productionTimestamp >= newTimestamp) {
				Log(LogInformation, "ApiListener")
					<< "Our configuration is more recent than the received configuration update."
					<< " Ignoring configuration file update for path '" << stageConfigZoneDir << "'. Current timestamp '"
					<< Utility::FormatDateTime("%Y-%m-%d %H:%M:%S %z", productionTimestamp) << "' ("
					<< std::fixed << std::setprecision(6) << productionTimestamp
					<< ") >= received timestamp '"
					<< Utility::FormatDateTime("%Y-%m-%d %H:%M:%S %z", newTimestamp) << "' ("
					<< newTimestamp << ").";
			} else {
				configChange = true;
			}

			/* Keep another hack when there's a timestamp file missing. */
			ObjectLock olock(newConfig);
			for (const Dictionary::Pair& kv : newConfig) {
				/* This is super expensive with a string content comparison. */
				if (productionConfig->Get(kv.first) != kv.second) {
					if (!Utility::Match("*/.timestamp", kv.first))
						configChange = true;
				}
			}

			/* Update the .timestamp file. */
			String tsPath = stageConfigZoneDir + "/.timestamp";
			if (!Utility::PathExists(tsPath)) {
				std::ofstream fp(tsPath.CStr(), std::ofstream::out | std::ostream::trunc);
				fp << std::fixed << newTimestamp;
				fp.close();
			}
		}

		/* Dump the received configuration for this zone into the stage directory. */
		size_t numBytes = 0;

		{
			ObjectLock olock(newConfig);
			for (const Dictionary::Pair& kv : newConfig) {
				/* Ignore same config content. This is an expensive comparison. */
				if (productionConfig->Get(kv.first) == kv.second)
					continue;

				/* Store the relative config file path for later validation and activation. */
				relativePaths.push_back(zoneName + "/" + kv.first);

				String path = stageConfigZoneDir + "/" + kv.first;

				Log(LogInformation, "ApiListener")
					<< "Stage: Updating received configuration file '" << path << "' for zone '" << zoneName << "'.";

				/* Sync string content only. */
				String content = kv.second;

				/* Generate a directory tree (zones/1/2/3 might not exist yet). */
				Utility::MkDirP(Utility::DirName(path), 0755);
				std::ofstream fp(path.CStr(), std::ofstream::out | std::ostream::binary | std::ostream::trunc);
				fp << content;
				fp.close();

				numBytes += content.GetLength();
			}
		}

		Log(LogInformation, "ApiListener")
			<< "Applying configuration file update for path '" << stageConfigZoneDir << "' ("
			<< numBytes << " Bytes).";

		/* If the update removes a path, delete it on disk and signal a config change. */
		{
			ObjectLock xlock(productionConfig);
			for (const Dictionary::Pair& kv : productionConfig) {
				if (!newConfig->Contains(kv.first)) {
					configChange = true;

					String path = stageConfigZoneDir + "/" + kv.first;
					(void) unlink(path.CStr());
				}
			}
		}
	}

	if (configChange) {
		/* Spawn a validation process. On success, move the staged configuration
		 * into production and restart.
		 */
		AsyncTryActivateZonesStage(relativePaths);
	}

	return Empty;
}

/**
 * Callback for stage config validation.
 * When validation was successful, the configuration is copied from
 * stage to production and a restart is triggered.
 * On failure, there's no restart and this is logged.
 *
 * @param pr Result of the validation process.
 * @param relativePaths Collected paths including the zone name, which are copied from stage to current directories.
 */
void ApiListener::TryActivateZonesStageCallback(const ProcessResult& pr,
	const std::vector<String>& relativePaths)
{
	String apiZonesDir = GetApiZonesDir();
	String apiZonesStageDir = GetApiZonesStageDir();

	String logFile = apiZonesStageDir + "/startup.log";
	std::ofstream fpLog(logFile.CStr(), std::ofstream::out | std::ostream::binary | std::ostream::trunc);
	fpLog << pr.Output;
	fpLog.close();

	String statusFile = apiZonesStageDir + "/status";
	std::ofstream fpStatus(statusFile.CStr(), std::ofstream::out | std::ostream::binary | std::ostream::trunc);
	fpStatus << pr.ExitStatus;
	fpStatus.close();

	/* validation went fine, copy stage and reload */
	if (pr.ExitStatus == 0) {
		Log(LogInformation, "ApiListener")
			<< "Config validation for stage '" << apiZonesStageDir << "' was OK, replacing into '" << apiZonesDir << "' and triggering reload.";

		/* Purge production before copying stage. */
		if (Utility::PathExists(apiZonesDir))
			Utility::RemoveDirRecursive(apiZonesDir);

		Utility::MkDirP(apiZonesDir, 0700);

		/* Copy all synced configuration files from stage to production. */
		for (const String& path : relativePaths) {
			Log(LogNotice, "ApiListener")
				<< "Copying file '" << path << "' from config sync staging to production zones directory.";

			String stagePath = apiZonesStageDir + path;
			String currentPath = apiZonesDir + path;

			Utility::MkDirP(Utility::DirName(currentPath), 0700);

			Utility::CopyFile(stagePath, currentPath);
		}

		ApiListener::Ptr listener = ApiListener::GetInstance();

		if (listener)
			listener->ClearLastFailedZonesStageValidation();

		Application::RequestRestart();

		return;
	}

	/* Error case. */
	Log(LogCritical, "ApiListener")
		<< "Config validation failed for staged cluster config sync in '" << apiZonesStageDir
		<< "'. Aborting. Logs: '" << logFile << "'";

	ApiListener::Ptr listener = ApiListener::GetInstance();

	if (listener)
		listener->UpdateLastFailedZonesStageValidation(pr.Output);
}

/**
 * Spawns a new validation process and waits for its output.
 * Sets 'System.ZonesStageVarDir' to override the config validation zone dirs with our current stage.
 *
 * @param relativePaths Required for later file operations in the callback. Provides the zone name plus path in a list.
 */
void ApiListener::AsyncTryActivateZonesStage(const std::vector<String>& relativePaths)
{
	VERIFY(Application::GetArgC() >= 1);

	/* Inherit parent process args. */
	Array::Ptr args = new Array({
		Application::GetExePath(Application::GetArgV()[0]),
	});

	for (int i = 1; i < Application::GetArgC(); i++) {
		String argV = Application::GetArgV()[i];

		if (argV == "-d" || argV == "--daemonize")
			continue;

		args->Add(argV);
	}

	args->Add("--validate");

	/* Set the ZonesStageDir. This creates our own local chroot without any additional automated zone includes. */
	args->Add("--define");
	args->Add("System.ZonesStageVarDir=" + GetApiZonesStageDir());

	Process::Ptr process = new Process(Process::PrepareCommand(args));
	process->SetTimeout(300);
	process->Run(std::bind(&TryActivateZonesStageCallback, _1, relativePaths));
}

/**
 * Update the structure from the last failed validation output.
 * Uses the current timestamp.
 *
 * @param log The process output from the config validation.
 */
void ApiListener::UpdateLastFailedZonesStageValidation(const String& log)
{
	Dictionary::Ptr lastFailedZonesStageValidation = new Dictionary({
		{ "log", log },
		{ "ts", Utility::GetTime() }
	});

	SetLastFailedZonesStageValidation(lastFailedZonesStageValidation);
}

/**
 * Clear the structure for the last failed reload.
 *
 */
void ApiListener::ClearLastFailedZonesStageValidation()
{
	SetLastFailedZonesStageValidation(Dictionary::Ptr());
}

/**
 * Generate a config checksum.
 *
 * @param content String content used for generating the checksum.
 * @returns The checksum as string.
 */
String ApiListener::GetChecksum(const String& content)
{
	return SHA256(content);
}

String ApiListener::GetGlobalChecksum(const ConfigDirInformation& config)
{
	Dictionary::Ptr checksums = config.Checksums;

	String result;

	ObjectLock olock(checksums);
	for (const Dictionary::Pair& kv : checksums) {
		String path = kv.first;
		String checksum = kv.second;

		/* Only use configuration files for checksum calculation. */
		//if (!Utility::Match("*.conf", path))
		//	continue;

		/* Ignore internal files, especially .timestamp and .checksums.
		 * If we don't, this results in "always change" restart loops.
		 */
		if (Utility::Match("/.*", path))
			continue;

		Log(LogCritical, "ApiListener")
			<< "Adding checksum for " << kv.first << ": " << kv.second;
		result += kv.second;
	}

	return GetChecksum(result);
}

/**
 * Load the given config dir and read their file content into the config structure.
 *
 * @param dir Path to the config directory.
 * @returns ConfigDirInformation structure.
 */
ConfigDirInformation ApiListener::LoadConfigDir(const String& dir)
{
	ConfigDirInformation config;
	config.UpdateV1 = new Dictionary();
	config.UpdateV2 = new Dictionary();
	config.Checksums = new Dictionary();

	Utility::GlobRecursive(dir, "*", std::bind(&ApiListener::ConfigGlobHandler, std::ref(config), dir, _1), GlobFile);
	return config;
}

/**
 * Read the given file and store it in the config information structure.
 * Callback function for Glob().
 *
 * @param config Reference to the config information object.
 * @param path File path.
 * @param file Full file name.
 */
void ApiListener::ConfigGlobHandler(ConfigDirInformation& config, const String& path, const String& file)
{
	CONTEXT("Creating config update for file '" + file + "'");

	Log(LogNotice, "ApiListener")
		<< "Creating config update for file '" << file << "'.";

	std::ifstream fp(file.CStr(), std::ifstream::binary);
	if (!fp)
		return;

	String content((std::istreambuf_iterator<char>(fp)), std::istreambuf_iterator<char>());

	Dictionary::Ptr update;
	String relativePath = file.SubStr(path.GetLength());

	/*
	 * 'update' messages contain conf files. 'update_v2' syncs everything else (.timestamp).
	 *
	 * **Keep this intact to stay compatible with older clients.**
	 */
	if (Utility::Match("*.conf", file))
		update = config.UpdateV1;
	else
		update = config.UpdateV2;

	update->Set(relativePath, content);

	/* Calculate a checksum for each file (and a global one later). */
	config.Checksums->Set(relativePath, GetChecksum(content));
}

/**
 * Compatibility helper for merging config update v1 and v2 into a global result.
 *
 * @param config Config information structure.
 * @returns Dictionary which holds the merged information.
 */
Dictionary::Ptr ApiListener::MergeConfigUpdate(const ConfigDirInformation& config)
{
	Dictionary::Ptr result = new Dictionary();

	if (config.UpdateV1)
		config.UpdateV1->CopyTo(result);

	if (config.UpdateV2)
		config.UpdateV2->CopyTo(result);

	return result;
}

