// Adapted from github.com/hedge-dev/UnleashedRecomp

# Dumping

> [!NOTE]
> The following guide is for dumping the game using a USB storage device.
>
> If you wish to dump the game from your Xbox 360 hard drive, use [this guide](DUMPING-en.md) instead.

### Pre-requisites
- Xbox 360 (modifications not necessary)
- USB Storage Device (16 GB minimum)
- Spider-Man: Edge of Time for Xbox 360 (US)
    - Retail Disc or Digital Copy (the Games on Demand release was delisted in 2014, so a digital copy has to be one you already own).
    - Title Update required.
    - The Identity Crisis Suits DLC is optional.
- [7-Zip](https://7-zip.org/download.html) (for extracting Velocity)
- [Velocity](https://github.com/Gualdimar/Velocity/releases/download/xex%2Biso-branch/Velocity-XEXISO.rar) (Gualdimar's fork)

> [!NOTE]
> Some Xbox 360 S|E consoles come with 4 GB of internal flash storage. These are **not compatible** with this method, since you cannot set a USB storage device as a system drive on them.
>
> An exception is for versions with the Trinity motherboard, where the storage module is located on a separate daughterboard and can be disconnected (not recommended for inexperienced users). On other models, this module is soldered directly onto the motherboard and cannot be disconnected.

### Instructions

1. Turn off your Xbox 360 and remove the hard drive from your console.

> [!TIP]
> You may consult the following guides if you're unsure on how to do this:
> - [Xbox 360](https://www.ifixit.com/Guide/Xbox+360+Hard+Drive+Replacement/3326)
> - [Xbox 360 S](https://www.ifixit.com/Guide/Xbox+360+S+Hard+Drive+Replacement/3184)
> - [Xbox 360 E](https://www.ifixit.com/Guide/Xbox+360+E+Hard+Drive+Replacement/22179)

2. Connect a USB storage device to the console and turn it on.
3. At the Xbox Dashboard, navigate to the **settings** tab, select **System**, then **Storage**.
4. On the **Storage Devices** screen, select your USB storage device and press Y to open **Device Options**.
5. Select **Make System Drive**, then **Configure Now** and confirm by selecting **Yes**.

> [!CAUTION]
> This operation will erase all content on the USB storage device. Before continuing, make sure that you have backed up its contents. **This action is irreversible!**

6. After successfully configuring the USB storage device as a system drive, turn off your console and reconnect the hard drive.
7. Turn on your console, then navigate to the **settings** tab, select **System**, then **Storage**.
8. Select the original hard drive, then navigate to the **Profiles** directory and move an Xbox Live profile that owns the game and its DLC to the USB storage device.
9. Turn off the console and remove the hard drive.
10. Turn on the console, **install the system update if prompted**, then sign in to your Xbox Live profile.

> [!NOTE]
> If you have a digital copy of Spider-Man: Edge of Time, skip to step 14.

11. Insert your retail disc copy of Spider-Man: Edge of Time into the Xbox 360 disc tray.
12. At the Xbox Dashboard, go over to the disc tile under the **home** tab and press X to view **Game Details**.
13. Under the **overview** tab, select the **Install** tile and choose to install to the USB drive.
14. For downloadable content, return to the Xbox Dashboard and navigate to the **settings** tab, select **Account**, then **Download History**. Look for the game (if you own it digitally) and the Identity Crisis Suits DLC. Select each one and choose **Download Again**.

> [!TIP]
> If the game or its DLC do not appear in **Download History**, make sure you moved the profile that owns them and are signed in to it.

15. Once installed, launch the game while connected to Xbox Live. If the title update isn't installed yet, you'll be prompted to install it.

> [!TIP]
> If you're unsure whether the title update is installed, clear the system cache (available in **Storage > Device Options**) and relaunch the game while connected to Xbox Live. This forces the title update to download again.

16. Turn off the console and connect the USB storage device to your PC.
17. Download [the latest release of Velocity](https://github.com/Gualdimar/Velocity/releases/download/xex%2Biso-branch/Velocity-XEXISO.rar) and open the `*.rar` file using [7-Zip](https://7-zip.org/download.html), then extract its contents anywhere that's convenient to you.
18. Create a new folder anywhere that's convenient to you for storing the game files.

> [!NOTE]
> If you're using Linux, skip to step 20.

19. Right-click `Velocity.exe` and click **Properties**, then under the **Compatibility** tab, tick **Run this program as an administrator** and click **OK**. This is required for Velocity to recognize the storage device.
20. Launch `Velocity.exe`. You should see a **Device Detected** message asking if you would like to open the **Device Content Viewer**. Click **Yes**.
21. You should now see a tree view of your storage device's contents. Expand the tree nodes for `/Shared Items/Games/` (and `/Shared Items/DLC/`, if you have the DLC installed).
22. Hold the CTRL key and click on **Spider-Man: Edge of Time** under the `Games` node (Velocity may list it as **Spider-Man™: EoT**), as well as **Identity Crisis Suits** under the `DLC` node, if you have the DLC installed. Make sure everything is selected before the next step.
23. Right-click any of the selected items and click **Copy Selected to Local Disk**, then navigate to the folder you created in step 18 and select it. Velocity will now copy the game files to your PC.
24. Once the transfer is complete, close the **Device Content Viewer** window and navigate to **Tools > Device Tools > Raw Device Viewer**.
25. Navigate to `/System Cache/Cache/` and click the **Name** column to sort by name. Look for a file that begins with `TU_10LC25I`. This is the title update, which is required for installation.

> [!TIP]
> To check that this is the right file, double-click it to open it in **Package Viewer**. It should be named **Spider-Man Edge of Time Title Update #1**, with `Default.xexp` inside along with a folder called `Data`.
>
> Once you've verified it, close the **Package Viewer** window and proceed to the next step.

26. Right-click the file that begins with `TU_10LC25I` and click **Copy Selected to Local Disk**, then navigate to the folder you created in step 18 and select it. Velocity will now copy the title update to your PC.
27. Once the transfer is complete, you should have all of the files needed for installation. You can now reconnect your hard drive to your console and move your Xbox Live profile back from the USB storage device. After that, [return to the readme and continue to the next step](/README.md#how-to-install).
