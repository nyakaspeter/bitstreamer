// The window's layout as XAML. It is parsed when the window is created
// (XamlReader), so the app needs no XAML compile step; the code looks the
// named elements up and attaches the behaviour.
#pragma once

namespace layout {

constexpr int kMainWidth = 400;      // the main column, in device-independent pixels
constexpr int kSettingsWidth = 480;  // the settings pane that opens beside it
constexpr int kHeight = 680;
constexpr int kMinMainWidth = 340;   // the window cannot be made narrower or shorter than this
constexpr int kMinHeight = 540;

inline const wchar_t* const kWindow = LR"xaml(
<Grid xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
      xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml">
    <Grid.ColumnDefinitions>
        <ColumnDefinition Width="*" />
        <ColumnDefinition Width="Auto" />
    </Grid.ColumnDefinitions>
    <Grid.RowDefinitions>
        <RowDefinition Height="44" />
        <RowDefinition Height="*" />
    </Grid.RowDefinitions>

    <!-- Settings pane: opens beside the main column. It starts at the very top
         so its background also runs behind the window buttons. -->
    <Border x:Name="settingsPane" Grid.Column="1" Grid.RowSpan="2" Visibility="Collapsed" Width="480"
            Background="{ThemeResource LayerFillColorDefaultBrush}"
            BorderBrush="{ThemeResource CardStrokeColorDefaultBrush}" BorderThickness="1,0,0,0">
        <ScrollViewer Margin="0,44,0,0" VerticalScrollBarVisibility="Auto" HorizontalScrollBarVisibility="Disabled">
            <StackPanel x:Name="settingsContent" Margin="24,6,24,24" Spacing="12">
                <StackPanel.RenderTransform>
                    <TranslateTransform x:Name="settingsShift" />
                </StackPanel.RenderTransform>
                <Border CornerRadius="8" Padding="16,14,16,16" BorderThickness="1"
                        Background="{ThemeResource CardBackgroundFillColorDefaultBrush}"
                        BorderBrush="{ThemeResource CardStrokeColorDefaultBrush}">
                    <Grid ColumnSpacing="12" RowSpacing="10">
                        <Grid.ColumnDefinitions>
                            <ColumnDefinition Width="Auto" />
                            <ColumnDefinition Width="*" />
                            <ColumnDefinition Width="*" />
                        </Grid.ColumnDefinitions>
                        <Grid.RowDefinitions>
                            <RowDefinition Height="Auto" />
                            <RowDefinition Height="Auto" />
                            <RowDefinition Height="Auto" />
                        </Grid.RowDefinitions>
                        <TextBlock Grid.Column="1" Text="Dolby Digital" Style="{ThemeResource CaptionTextBlockStyle}"
                                   Foreground="{ThemeResource TextFillColorSecondaryBrush}" />
                        <TextBlock Grid.Column="2" Text="DTS" Style="{ThemeResource CaptionTextBlockStyle}"
                                   Foreground="{ThemeResource TextFillColorSecondaryBrush}" />
                        <TextBlock Grid.Row="1" Text="Bitrate" VerticalAlignment="Center" />
                        <ComboBox x:Name="ac3Bitrate" Grid.Row="1" Grid.Column="1" HorizontalAlignment="Stretch" />
                        <ComboBox x:Name="dtsBitrate" Grid.Row="1" Grid.Column="2" HorizontalAlignment="Stretch" />
                        <TextBlock Grid.Row="2" Text="Safety margin" VerticalAlignment="Center"
                                   ToolTipService.ToolTip="Spare time kept before the output needs each frame, in milliseconds. Lower means less delay but a higher risk of dropouts." />
                        <NumberBox x:Name="ac3Margin" Grid.Row="2" Grid.Column="1" Minimum="2" Maximum="30" SmallChange="1" LargeChange="1"
                                   SpinButtonPlacementMode="Inline" HorizontalAlignment="Stretch" />
                        <NumberBox x:Name="dtsMargin" Grid.Row="2" Grid.Column="2" Minimum="2" Maximum="30" SmallChange="1" LargeChange="1"
                                   SpinButtonPlacementMode="Inline" HorizontalAlignment="Stretch" />
                    </Grid>
                </Border>
                <Border CornerRadius="8" BorderThickness="1"
                        Background="{ThemeResource CardBackgroundFillColorDefaultBrush}"
                        BorderBrush="{ThemeResource CardStrokeColorDefaultBrush}">
                    <StackPanel>
                        <Grid Padding="16,6,6,6" MinHeight="48">
                            <Grid.ColumnDefinitions><ColumnDefinition Width="*" /><ColumnDefinition Width="Auto" /></Grid.ColumnDefinitions>
                            <TextBlock Text="Start with Windows" VerticalAlignment="Center" TextWrapping="Wrap" Margin="0,0,20,0" />
                            <ToggleSwitch x:Name="startWithWindows" Grid.Column="1" MinWidth="0" OnContent="" OffContent="" />
                        </Grid>
                        <Border Height="1" Background="{ThemeResource DividerStrokeColorDefaultBrush}" />
                        <Grid Padding="16,6,6,6" MinHeight="48">
                            <Grid.ColumnDefinitions><ColumnDefinition Width="*" /><ColumnDefinition Width="Auto" /></Grid.ColumnDefinitions>
                            <TextBlock Text="Start encoding when bitstreamer starts" VerticalAlignment="Center" TextWrapping="Wrap" Margin="0,0,20,0" />
                            <ToggleSwitch x:Name="startOnLaunch" Grid.Column="1" MinWidth="0" OnContent="" OffContent="" />
                        </Grid>
                        <Border Height="1" Background="{ThemeResource DividerStrokeColorDefaultBrush}" />
                        <Grid Padding="16,6,6,6" MinHeight="48"
                              ToolTipService.ToolTip="On: starting makes the Steam device the Windows playback device and stopping switches back. Off: bitstreamer never changes the playback device; it encodes whenever you select the Steam device yourself.">
                            <Grid.ColumnDefinitions><ColumnDefinition Width="*" /><ColumnDefinition Width="Auto" /></Grid.ColumnDefinitions>
                            <TextBlock Text="Switch to Steam Streaming Speakers when encoding starts" VerticalAlignment="Center" TextWrapping="Wrap" Margin="0,0,20,0" />
                            <ToggleSwitch x:Name="switchToSteam" Grid.Column="1" MinWidth="0" OnContent="" OffContent="" />
                        </Grid>
                        <Border Height="1" Background="{ThemeResource DividerStrokeColorDefaultBrush}" />
                        <Grid Padding="16,6,6,6" MinHeight="48">
                            <Grid.ColumnDefinitions><ColumnDefinition Width="*" /><ColumnDefinition Width="Auto" /></Grid.ColumnDefinitions>
                            <TextBlock Text="Keep running in the notification area when the window is closed" VerticalAlignment="Center" TextWrapping="Wrap" Margin="0,0,20,0" />
                            <ToggleSwitch x:Name="keepInTray" Grid.Column="1" MinWidth="0" OnContent="" OffContent="" />
                        </Grid>
                    </StackPanel>
                </Border>
                <Border CornerRadius="8" Padding="16,12,16,14" BorderThickness="1"
                        Background="{ThemeResource CardBackgroundFillColorDefaultBrush}"
                        BorderBrush="{ThemeResource CardStrokeColorDefaultBrush}">
                    <Grid x:Name="details" ColumnSpacing="20" RowSpacing="5">
                        <Grid.ColumnDefinitions>
                            <ColumnDefinition Width="Auto" />
                            <ColumnDefinition Width="*" />
                        </Grid.ColumnDefinitions>
                    </Grid>
                </Border>
            </StackPanel>
        </ScrollViewer>
    </Border>

    <!-- Title bar: the drag area of the window. -->
    <Grid x:Name="titleBar" Grid.ColumnSpan="2" Background="Transparent">
        <StackPanel Orientation="Horizontal" Margin="18,0,0,0" VerticalAlignment="Center" Spacing="10">
            <!-- The app icon, drawn as shapes so it stays sharp at any scaling. -->
            <Viewbox Width="18" Height="18">
                <Grid Width="16" Height="16">
                    <Border CornerRadius="3.5" Background="#FF1C2434" />
                    <StackPanel Orientation="Horizontal" HorizontalAlignment="Center" VerticalAlignment="Bottom" Margin="0,0,0,2.9" Spacing="0.6">
                        <Rectangle Width="1.1" Height="5.6" RadiusX="0.5" RadiusY="0.5" Fill="#FF60CDFF" VerticalAlignment="Bottom" />
                        <Rectangle Width="1.1" Height="8.2" RadiusX="0.5" RadiusY="0.5" Fill="#FF60CDFF" VerticalAlignment="Bottom" />
                        <Rectangle Width="1.1" Height="4.6" RadiusX="0.5" RadiusY="0.5" Fill="#FF60CDFF" VerticalAlignment="Bottom" />
                        <Rectangle Width="1.1" Height="7.2" RadiusX="0.5" RadiusY="0.5" Fill="#FF60CDFF" VerticalAlignment="Bottom" />
                        <Rectangle Width="1.1" Height="3.6" RadiusX="0.5" RadiusY="0.5" Fill="#FF60CDFF" VerticalAlignment="Bottom" />
                        <Rectangle Width="1.1" Height="6.1" RadiusX="0.5" RadiusY="0.5" Fill="#FF60CDFF" VerticalAlignment="Bottom" />
                    </StackPanel>
                </Grid>
            </Viewbox>
            <TextBlock Text="bitstreamer" Style="{ThemeResource CaptionTextBlockStyle}" VerticalAlignment="Center" />
        </StackPanel>
    </Grid>

    <!-- Main column. -->
    <Grid Grid.Row="1" Margin="24,6,24,24" RowSpacing="14" MaxWidth="560">
        <Grid.RowDefinitions>
            <RowDefinition Height="*" />
            <RowDefinition Height="Auto" />
            <RowDefinition Height="Auto" />
            <RowDefinition Height="Auto" />
            <RowDefinition Height="Auto" />
        </Grid.RowDefinitions>

        <Border CornerRadius="8" BorderThickness="1" Padding="10,22,10,12"
                Background="{ThemeResource CardBackgroundFillColorDefaultBrush}"
                BorderBrush="{ThemeResource CardStrokeColorDefaultBrush}">
            <Grid>
                <Grid x:Name="bars">
                    <Grid.ColumnDefinitions>
                        <ColumnDefinition Width="*" /><ColumnDefinition Width="*" /><ColumnDefinition Width="*" />
                        <ColumnDefinition Width="*" /><ColumnDefinition Width="*" /><ColumnDefinition Width="*" />
                    </Grid.ColumnDefinitions>
                </Grid>
                <TextBlock x:Name="message" TextWrapping="Wrap" TextAlignment="Center" HorizontalAlignment="Center"
                           VerticalAlignment="Center" Margin="28,0,28,30" />
            </Grid>
        </Border>

        <ComboBox x:Name="output" Grid.Row="1" Header="Output" HorizontalAlignment="Stretch" />
        <ComboBox x:Name="format" Grid.Row="2" Header="Format" HorizontalAlignment="Stretch" />
        <ComboBox x:Name="upmix" Grid.Row="3" Header="Upmix" HorizontalAlignment="Stretch"
                  ToolTipService.ToolTip="What to do with stereo sound. Real 5.1 sound is never changed." />

        <Grid Grid.Row="4" ColumnSpacing="8" Margin="0,10,0,0">
            <Grid.ColumnDefinitions>
                <ColumnDefinition Width="*" />
                <ColumnDefinition Width="Auto" />
            </Grid.ColumnDefinitions>
            <Button x:Name="start" Height="40" HorizontalAlignment="Stretch" Style="{ThemeResource AccentButtonStyle}">
                <StackPanel Orientation="Horizontal" Spacing="8">
                    <FontIcon x:Name="startIcon" Glyph="&#xE768;" FontSize="14" />
                    <TextBlock x:Name="startLabel" Text="Start" />
                </StackPanel>
            </Button>
            <ToggleButton x:Name="settingsToggle" Grid.Column="1" Width="40" Height="40" Padding="0"
                          AutomationProperties.Name="Settings"
                          ToolTipService.ToolTip="Settings">
                <FontIcon Glyph="&#xE713;" FontSize="16" />
            </ToggleButton>
        </Grid>
    </Grid>
</Grid>
)xaml";

// One channel of the level meter: a rounded track, the level bar growing
// from its bottom, and the label.
inline const wchar_t* const kMeterBar = LR"xaml(
<Grid xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
      xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml">
    <Grid.RowDefinitions>
        <RowDefinition Height="*" />
        <RowDefinition Height="Auto" />
    </Grid.RowDefinitions>
    <Grid x:Name="track" Width="16" HorizontalAlignment="Center">
        <Border CornerRadius="8" Background="{ThemeResource TextFillColorPrimaryBrush}" Opacity="0.07" />
        <Border x:Name="fill" CornerRadius="8" VerticalAlignment="Bottom" Height="0">
            <Border.Background>
                <LinearGradientBrush StartPoint="0,1" EndPoint="0,0">
                    <GradientStop Color="{ThemeResource SystemAccentColorDark1}" Offset="0" />
                    <GradientStop Color="{ThemeResource SystemAccentColorLight2}" Offset="1" />
                </LinearGradientBrush>
            </Border.Background>
        </Border>
        <Border x:Name="hot" CornerRadius="8" VerticalAlignment="Bottom" Height="0" Opacity="0">
            <Border.Background>
                <LinearGradientBrush StartPoint="0,1" EndPoint="0,0">
                    <GradientStop Color="{ThemeResource SystemAccentColorDark1}" Offset="0" />
                    <GradientStop Color="#FFF0AD4E" Offset="0.8" />
                    <GradientStop Color="#FFE84855" Offset="1" />
                </LinearGradientBrush>
            </Border.Background>
        </Border>
    </Grid>
    <TextBlock x:Name="label" Grid.Row="1" Margin="0,10,0,0" HorizontalAlignment="Center"
               Style="{ThemeResource CaptionTextBlockStyle}"
               Foreground="{ThemeResource TextFillColorSecondaryBrush}" />
</Grid>
)xaml";

}  // namespace layout
