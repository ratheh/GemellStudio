# External Node Service Examples

This directory contains an example service manifest file for the External Geometry Node Discovery Service system.

## Service Discovery Overview

The External Node Service system uses a **simple service discovery** approach:
1. Installers write service manifest files to a system-level directory
2. Blender reads these manifests on startup to discover available services
3. Future phases will launch services and communicate via REST API

**No user configuration files are generated** - Blender reads manifests directly from the system directory.

## Files

### gemell-geo-nodes-manifest.json (Example Manifest)
This is an example **service manifest** file that would be installed by an external service installer (like FabricDataLoaderAPI) to:
- **Windows**: `%PROGRAMDATA%\GemellStudio\ExternalServices\gemell-geo-nodes-manifest.json`
- **Linux**: `/usr/share/GemellStudio/external_services/gemell-geo-nodes-manifest.json`
- **macOS**: `/Library/Application Support/GemellStudio/ExternalServices/gemell-geo-nodes-manifest.json`

The manifest describes:
- Service identification (ID, name, version, vendor)
- Installation details (where the service executable is installed)
- Service configuration (API settings, UI settings, IPC settings)
- Lifecycle settings (auto-start, restart behavior)

## Usage

### For Installer Developers (FabricDataLoaderAPI)
When creating an installer for your external service:
1. Use `gemell-geo-nodes-manifest.json` as a template for your manifest
2. Update paths, service ID, name, and other details to match your service
3. Install the manifest to the system-level directory during installation
4. That's it! Blender will discover it on next startup

### For Testing
To test the external service discovery system:
1. Copy `gemell-geo-nodes-manifest.json` to your system's manifest directory
2. Update the `executable_path` to point to an actual executable (or create a dummy one)
3. Launch GemellStudio/Blender with logging enabled (see below)
4. Check the console logs for discovery messages

#### Viewing Logs During Testing
The External Node Service Manager uses Blender's CLG (C Logging) system. To see log messages, you must launch GemellStudio/Blender from the command line with logging enabled.

**Enable logging for the external service system only:**
```bash
# Windows
cd D:\bdev\build_windows_x64_vc17_Release\bin\Release
.\blender.exe --log "bke.node_external_service" --log-level 1

# Linux/macOS
./blender --log "bke.node_external_service" --log-level 1
```

**Enable all Blender logs (more verbose):**
```bash
.\blender.exe --log "*" --log-level 1
```

**Log Levels:**
- `0` = Info (default)
- `1` = Info (more detailed)
- `2` = Info (very verbose)
- `-1` = Warnings only
- `-2` = Errors only

**Expected Log Messages:**
When the discovery system initializes, you should see messages like:
- `Initializing External Node Service Manager...`
- `Scanning manifest directory: C:\ProgramData\GemellStudio\ExternalServices`
- `Found X manifest file(s)`
- `Successfully parsed manifest: ... (service: ...)`
- `Loaded X external service(s)`

If no manifests are found:
- `Manifest directory does not exist: ...` or
- `No external service manifests found`

## Manifest Schema

### Service Manifest File Structure
```json
{
  "manifest_version": "1.0",
  "service": {
    "id": "unique.service.id",
    "name": "Display Name",
    "version": "1.0.0",
    "description": "Service description",
    "vendor": "Vendor Name"
  },
  "installation": {
    "install_root": "/path/to/install",
    "executable_path": "/path/to/executable",
    "working_directory": "/path/to/workdir"
  },
  "service_config": {
    "executable": {
      "args": ["--arg1", "value1"]
    },
    "api": {
      "protocol": "http",
      "host": "127.0.0.1",
      "port": 0,
      "base_path": "/api/v1/geonodes",
      "startup_timeout_ms": 10000,
      "health_check_endpoint": "/health"
    },
    "node_collection": {
      "menu_name": "Menu Name",
      "icon": "ICON_NAME",
      "description": "Description"
    },
    "shared_memory": {
      "format": "apache_arrow",
      "alignment": 64
    },
    "auto_start": 0,
    "restart_on_failure": 1,
    "max_restart_attempts": 3
  }
}
```

### Important Notes
- The `{port}` placeholder in args will be replaced with the actual port at runtime (future phase)
- Service `id` should use reverse domain notation (e.g., `com.company.service`)
- `port: 0` means dynamic port allocation by Blender
- All paths should be absolute paths to ensure proper discovery

## How It Works

1. **Installation**: Your installer writes a manifest file to the system directory
2. **Discovery**: Blender scans the system directory on startup and loads all valid manifests
3. **Future**: In later phases, Blender will launch services and call their REST APIs to discover geometry nodes
