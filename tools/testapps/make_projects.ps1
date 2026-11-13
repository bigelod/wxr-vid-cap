# Generates one vcxproj per capture test app. The projects share testapp.props and differ only in
# the source file, output name, and (for Vulkan) the SDK paths.
#
# Every app except Vulkan gets an x64 build (<name>.exe) and a Win32 build (<name>32.exe). The
# container's D3D8 runtime is 32-bit, and the runner exercises both bitnesses.

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot

$apps = @(
    @{ Name = "cube_d3d7";  Guid = "C0BE0007-1111-4A01-9000-000000000007"; Namespace = "cubetestd3d7" },
    @{ Name = "cube_d3d8";  Guid = "C0BE0008-1111-4A01-9000-000000000008"; Namespace = "cubetestd3d8" },
    @{ Name = "cube_d3d9";  Guid = "C0BE0009-1111-4A01-9000-000000000009"; Namespace = "cubetestd3d9" },
    @{ Name = "cube_d3d10"; Guid = "C0BE0010-1111-4A01-9000-000000000010"; Namespace = "cubetestd3d10" },
    @{ Name = "cube_d3d11"; Guid = "C0BE0011-1111-4A01-9000-000000000011"; Namespace = "cubetestd3d11" },
    @{ Name = "cube_d3d12"; Guid = "C0BE0012-1111-4A01-9000-000000000012"; Namespace = "cubetestd3d12" },
    @{ Name = "cube_gl";    Guid = "C0BE0013-1111-4A01-9000-000000000013"; Namespace = "cubetestgl" },
    @{ Name = "cube_vk";    Guid = "C0BE0014-1111-4A01-9000-000000000014"; Namespace = "cubetestvk" }
)

$vulkanExtra = @"
  <PropertyGroup>
    <VulkanSdkDir Condition="'`$(VulkanSdkDir)'==''">D:\VulkanSDK\1.4.350.0</VulkanSdkDir>
  </PropertyGroup>
  <ItemDefinitionGroup>
    <ClCompile>
      <AdditionalIncludeDirectories>`$(VulkanSdkDir)\Include;%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories>
    </ClCompile>
    <Link>
      <AdditionalLibraryDirectories>`$(VulkanSdkDir)\Lib;%(AdditionalLibraryDirectories)</AdditionalLibraryDirectories>
    </Link>
  </ItemDefinitionGroup>
"@

foreach ($app in $apps) {
    $extra = ""
    $win32Configs = ""
    $win32Props = ""

    if ($app.Name -eq "cube_vk") {
        $extra = $vulkanExtra
    } else {
        $win32Configs = @"
    <ProjectConfiguration Include="Debug|Win32">
      <Configuration>Debug</Configuration>
      <Platform>Win32</Platform>
    </ProjectConfiguration>
    <ProjectConfiguration Include="Release|Win32">
      <Configuration>Release</Configuration>
      <Platform>Win32</Platform>
    </ProjectConfiguration>
"@
        $win32Props = @"
  <PropertyGroup Condition="'`$(Platform)'=='Win32'">
    <OutDir>`$(MSBuildThisFileDirectory)..\..\build\testapps\Win32\`$(Configuration)\</OutDir>
    <IntDir>`$(MSBuildThisFileDirectory)..\..\build\obj\testapps\Win32\`$(Configuration)\$($app.Name)\</IntDir>
    <TargetName>$($app.Name)32</TargetName>
  </PropertyGroup>
"@
    }

    $content = @"
<?xml version="1.0" encoding="utf-8"?>
<Project DefaultTargets="Build" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <ItemGroup Label="ProjectConfigurations">
    <ProjectConfiguration Include="Debug|x64">
      <Configuration>Debug</Configuration>
      <Platform>x64</Platform>
    </ProjectConfiguration>
    <ProjectConfiguration Include="Release|x64">
      <Configuration>Release</Configuration>
      <Platform>x64</Platform>
    </ProjectConfiguration>
$win32Configs  </ItemGroup>
  <PropertyGroup Label="Globals">
    <VCProjectVersion>17.0</VCProjectVersion>
    <Keyword>Win32Proj</Keyword>
    <ProjectGuid>{$($app.Guid)}</ProjectGuid>
    <RootNamespace>$($app.Namespace)</RootNamespace>
    <WindowsTargetPlatformVersion>10.0</WindowsTargetPlatformVersion>
  </PropertyGroup>
  <Import Project="`$(VCTargetsPath)\Microsoft.Cpp.Default.props" />
  <Import Project="testapp.props" />
  <Import Project="`$(VCTargetsPath)\Microsoft.Cpp.props" />
  <ImportGroup Label="PropertySheets">
    <Import Project="`$(UserRootDir)\Microsoft.Cpp.`$(Platform).user.props" Condition="exists('`$(UserRootDir)\Microsoft.Cpp.`$(Platform).user.props')" Label="LocalAppDataPlatform" />
  </ImportGroup>
$extra  <PropertyGroup>
    <TargetName>$($app.Name)</TargetName>
  </PropertyGroup>
$win32Props  <ItemGroup>
    <ClCompile Include="common.cpp" />
    <ClCompile Include="$($app.Name).cpp" />
  </ItemGroup>
  <Import Project="`$(VCTargetsPath)\Microsoft.Cpp.targets" />
</Project>
"@
    Set-Content -Path (Join-Path $root "$($app.Name).vcxproj") -Value $content -Encoding UTF8
    Write-Host "wrote $($app.Name).vcxproj"
}
