# About

This project is a **Windows** desktop application for **Confo Quanta's laser writing product**. 
Operators can control the position of a stage by controlling the motors through the GUI and load an image for the device to draw. 

The application is written in **C++** with a user interface made using the **Qt** library. 

# Getting started

Install [Git for Windows](https://gitforwindows.org/) if needed. 

Open **Command Prompt** in the folder where you want to keep the project and run:

```bat
git clone https://github.com/JuntoM147/thorlabs-kdc101-control-app.git
cd thorlabs-kdc101-control-app
```

On 64-bit Windows, you will need:

- [Visual Studio 2022 or Build Tools 2022](https://visualstudio.microsoft.com/vs/older-downloads/) with **Desktop development with C++** (MSVC v143 and Windows SDK).
- [CMake 3.21 or newer](https://cmake.org/download/) on your PATH.
- [Qt 6.5 or newer](https://www.qt.io/download-qt-installer-oss) with the MSVC 64-bit desktop kit (Core, Gui, and Widgets).
- [Thorlabs Kinesis](https://www.thorlabs.com/software-pages/motion_control/) with the 64-bit benchtop stepper SDK and runtime.
- [NI-DAQmx](https://www.ni.com/en/support/downloads/drivers/download.ni-daq-mx.html) with the driver and C development support.

In File Explorer, open your Qt installation folder, then your Qt version, and copy the full path of the 64-bit MSVC kit folder containing `bin`, `include`, and `lib` (for example, `C:\Qt\6.11.2\msvc2022_64`) for both commands below.

To check you have installed all the dependencies, run this from the repository root in **Command Prompt**, using your Qt kit path:

```bat
powershell -NoProfile -ExecutionPolicy RemoteSigned -File tools\check_dependencies.ps1 -QtDir "C:/Qt/6.11.2/msvc2022_64"
```

To build, run this line in **Command Prompt** from the repository root with the same Qt kit path. CMake checks the compiler and development libraries before building the Release application.

```bat
cmake --preset app -DCMAKE_PREFIX_PATH="C:/Qt/6.11.2/msvc2022_64" && cmake --build --preset app
```

The executable is `build-app\Release\frontend.exe`. CMake must be on PATH; otherwise use a Visual Studio developer command prompt or add your CMake `bin` directory to PATH.

A fresh build disables laser output (`PCIE6323_LASER_MODE=unconfigured`). Existing build folders retain their hardware settings, including `digital` mode. Check [hardware profiles](docs/hardware.md) before packaging; to explicitly disable laser output, add `-DPCIE6323_LASER_MODE=unconfigured` to the configure command.

## Build the USB package

On the development PC, configure the `app` preset as above. From the repository root, run the command below with the path to Microsoft's **x64 Visual C++ Redistributable** installer. Use a version at least as recent as the MSVC toolsets used for the app and Qt. A Qt installation may already contain the installer under `C:\Qt\vcredist`.

```bat
powershell -NoProfile -ExecutionPolicy RemoteSigned -File tools\package_windows.ps1 -VcRedist "C:\Qt\vcredist\vc14.50.35719_VC_redist.x64.exe"
```

The script builds Release with the configured hardware settings and writes `build-app\Confo-Quanta-win-x64.zip`. It includes the executable, Qt DLLs and plugins, the Microsoft runtime installer, and a launcher. The packaged `READ ME.txt` records the stage travels and laser mode. The default `unconfigured` mode disables laser output. See the [hardware profiles](docs/hardware.md) before choosing a different mode.

## What to copy onto the USB

- `build-app\Confo-Quanta-win-x64.zip` (the complete application package).
- The **64-bit Thorlabs Kinesis installer**, if Kinesis is not already installed on the target PC.
- The **full NI-DAQmx offline installer** for the target Windows version and PCIe-6323, if NI-DAQmx is not already installed. A web installer is insufficient on an offline PC.
- Any images you intend to load for drawing.

The ZIP already includes Qt DLLs/plugins and `vc_redist.x64.exe`. Do not copy just `frontend.exe`; source files and the rest of the build directory are unnecessary.

## Run on the target PC

1. Install 64-bit Kinesis and NI-DAQmx if missing, and restart if their installers request it. Both runtimes are required at application startup, even when laser output is disabled.
2. Copy the ZIP from USB to a local folder and use **Extract All**. Keep all extracted files and subfolders together.
3. Open the extracted `Confo-Quanta` folder and run `vc_redist.x64.exe` once. Keep an equal or newer runtime if already installed.
4. Check `READ ME.txt` for the packaged stage travels, simulation setting, laser mode, and expected DAQ serial.
5. Double-click **Start Confo Quanta.bat**. It adds the default Kinesis directory to PATH for this launch. For a custom installation, add the folder containing `Thorlabs.MotionControl.Benchtop.StepperMotor.dll` to PATH first.
6. Select the controller and distinct axis channels in the GUI. For digital laser operation, follow the [hardware configuration](docs/hardware.md) before connecting the DAQ or starting a scan.

The target PC needs no Qt installation, compiler, or internet connection. If startup reports `nicaiu.dll` missing, install NI-DAQmx; for missing Kinesis DLLs, check the installation and PATH. Laser Quantum RemoteApp is a separate interface.

## Basic checks

Run the dependency checker above, then build Release. The checker verifies configuration and loads vendor DLLs without connecting to hardware. There are currently no automated CTest tests. On the target PC, launch through the packaged batch file and confirm the window opens; motion and laser operation require separate hardware validation.

# Documentation

For architecture and design justifications please refer to the [documentation here](docs/README.md)

# Coding Style

I have tried as much as possible to follow Google's C++ Coding Standard

See: https://google.github.io/styleguide/cppguide.html#C++_Version

# Layout Style

For project organisation I have tried to follow The Pitchfork Layout (PFL) convention

See: https://joholl.github.io/pitchfork-website/

# AI Usage

ChatGPT codex was used in this project to help identify bugs, suggest code fixes, and to discuss the underlying algorithm and structure of the project.
