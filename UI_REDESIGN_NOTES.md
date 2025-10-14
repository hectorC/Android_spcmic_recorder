# UI Redesign - Dashboard Layout (ui_design branch)

## Overview
Redesigned the Record tab from a vertical card list into a professional dashboard layout inspired by hardware recording equipment.

## Layout Structure (Hybrid - Option C)

### 1. **Top Hero Area - Large Timecode Display**
- **Prominent timecode** in large monospace font (64sp)
- Changes color when recording (grey → red)
- Recording filename displayed below
- Clean card with generous padding
- Focus on readability from distance

### 2. **Middle Section - Compact Status Row**
- **Three equal-width status tiles** in a horizontal row:
  1. **USB Connection** - Icon, status badge, connection text
  2. **Storage Location** - Icon, label, truncated path
  3. **Clip Indicator** - Icon, status text (pulses when clipping)
- Information-dense without scrolling
- Each tile is self-contained with icon + status

### 3. **Scrollable Info Section**
- **Device Info Card**:
  - Selected sample rate
  - Negotiated device rate
  - Device capabilities
- **Recording Configuration Card**:
  - 84 Channels
  - Sample rate
  - 24-bit depth
  - WAV format
- Compact, collapsible information
- Only shown when needed

### 4. **Bottom Control Panel**
- **Sample Rate Selector** with icon (left side)
- **Refresh Button** - Reconnect USB device (icon only)
- **Settings Button** - Opens bottom sheet (icon only)
- **Large Record/Stop Button** - Primary action, full-width, elevated

## Settings Bottom Sheet
- **Storage Location**:
  - Current path display
  - Change button → launches SAF picker
- **Audio Monitoring**:
  - Reset clip indicator button
- Clean, focused settings access
- Dismissible after action

## Design Principles Applied
✅ **Professional aesthetic** - Hardware-inspired, not consumer app  
✅ **Information density** - Maximum info visible at once  
✅ **Minimal scrolling** - Critical controls always visible  
✅ **Dark theme friendly** - Professional audio tool look  
✅ **Monospace typography** - Timecode and technical data  
✅ **Color coding** - Red for recording/clip, green for OK, primary for actions  
✅ **Focus hierarchy** - Timecode → Status → Controls  

## Key Features
- **No vertical card scrolling** - Everything accessible via dashboard layout
- **Collapsible details** - Advanced info hidden until needed
- **Quick actions** - Transport controls always at fingertips
- **Visual status** - At-a-glance system health (USB + Storage + Audio)
- **Professional UX** - Feels like operating recording hardware

## Files Changed
- `fragment_record_dashboard.xml` - New dashboard layout
- `RecordFragment.kt` - Updated to use new layout + bottom sheet
- `bottom_sheet_settings.xml` - Settings overlay
- `ic_refresh.xml`, `ic_settings.xml` - New icon resources
- `strings.xml` - Added "No Clip" short label

## Testing the Design
1. Checkout the `ui_design` branch:
   ```bash
   git checkout ui_design
   ```
2. Build and run the app
3. Observe the new dashboard layout on the Record tab
4. Test Settings button to access storage/clip options
5. Compare with main branch layout

## Switching Back to Original
```bash
git checkout main
```

## Next Steps (if approved)
- Fine-tune spacing and colors
- Add animation transitions
- Consider landscape layout optimization
- Add visual recording pulse to timecode area
- Test on various screen sizes
