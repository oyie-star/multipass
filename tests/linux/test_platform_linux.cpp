/*
 * Copyright (C) 2019-2021 Canonical, Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "tests/fake_handle.h"
#include "tests/file_operations.h"
#include "tests/mock_environment_helpers.h"
#include "tests/mock_process_factory.h"
#include "tests/mock_settings.h"
#include "tests/temp_dir.h"
#include "tests/test_with_mocked_bin_path.h"

#include <src/platform/backends/libvirt/libvirt_virtual_machine_factory.h>
#include <src/platform/backends/libvirt/libvirt_wrapper.h>
#include <src/platform/backends/lxd/lxd_virtual_machine_factory.h>
#include <src/platform/backends/qemu/qemu_virtual_machine_factory.h>
#include <src/platform/platform_linux_detail.h>

#include <multipass/constants.h>
#include <multipass/exceptions/autostart_setup_exception.h>
#include <multipass/exceptions/settings_exceptions.h>
#include <multipass/platform.h>

#include <QDir>
#include <QFile>
#include <QString>

#include <scope_guard.hpp>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <stdexcept>

namespace mp = multipass;
namespace mpt = multipass::test;
using namespace testing;

namespace
{
const auto backend_path = QStringLiteral("/tmp");

void setup_driver_settings(const QString& driver)
{
    auto& expectation = EXPECT_CALL(mpt::MockSettings::mock_instance(), get(Eq(mp::driver_key)));
    if (!driver.isEmpty())
        expectation.WillRepeatedly(Return(driver));
}

// hold on to return until the change is to be discarded
auto temporarily_change_env(const char* var_name, QByteArray var_value)
{
    auto guard = sg::make_scope_guard([var_name, var_save = qgetenv(var_name)]() noexcept { // terminate ok if it throws
        qputenv(var_name, var_save);
    });

    qputenv(var_name, var_value);

    return guard;
}

template <typename Guards>
struct autostart_test_record
{
    autostart_test_record(const QDir& autostart_dir, const QString& autostart_filename,
                          const QString& autostart_contents, Guards&& guards)
        : autostart_dir{autostart_dir},
          autostart_filename{autostart_filename},
          autostart_contents{autostart_contents},
          guards{std::forward<Guards>(guards)} // this should always move, since scope guards are not copyable
    {
    }

    QDir autostart_dir;
    QString autostart_filename;
    QString autostart_contents;
    Guards guards;
};

struct autostart_test_setup_error : public std::runtime_error
{
    using std::runtime_error::runtime_error;
};

auto setup_autostart_desktop_file_test()
{
    QDir test_dir{QDir::temp().filePath(QString{"%1_%2"}.arg(mp::client_name, "autostart_test"))};
    if (test_dir.exists() || QFile{test_dir.path()}.exists()) // avoid touching this at all if it already exists
        throw autostart_test_setup_error{fmt::format("Test dir or file already exists: {}", test_dir.path())}; /*
                           use exception to abort the test, since ASSERTS only work in void-returning functions */

    // Now mock filesystem tree and environment, reverting when done

    auto guard_fs = sg::make_scope_guard([test_dir]() mutable noexcept { // std::terminate ok if this throws
        test_dir.removeRecursively(); // succeeds if not there
    });

    QDir data_dir{test_dir.filePath("data")};
    QDir config_dir{test_dir.filePath("config")};
    auto guard_home = temporarily_change_env("HOME", "hide/me");
    auto guard_xdg_config = temporarily_change_env("XDG_CONFIG_HOME", config_dir.path().toLatin1());
    auto guard_xdg_data = temporarily_change_env("XDG_DATA_DIRS", data_dir.path().toLatin1());

    QDir mp_data_dir{data_dir.filePath(mp::client_name)};
    QDir autostart_dir{config_dir.filePath("autostart")};
    EXPECT_TRUE(mp_data_dir.mkpath("."));   // this is where the directories are actually created
    EXPECT_TRUE(autostart_dir.mkpath(".")); // idem

    const auto autostart_filename = mp::platform::autostart_test_data();
    const auto autostart_filepath = mp_data_dir.filePath(autostart_filename);
    const auto autostart_contents = "Exec=multipass.gui --autostarting\n";

    {
        QFile autostart_file{autostart_filepath};
        EXPECT_TRUE(autostart_file.open(QIODevice::WriteOnly)); // create desktop file to link against
        EXPECT_EQ(autostart_file.write(autostart_contents), qstrlen(autostart_contents));
    }

    auto guards = std::make_tuple(std::move(guard_home), std::move(guard_xdg_config), std::move(guard_xdg_data),
                                  std::move(guard_fs));
    return autostart_test_record{autostart_dir, autostart_filename, autostart_contents, std::move(guards)};
}

void check_autostart_file(const QDir& autostart_dir, const QString& autostart_filename,
                          const QString& autostart_contents)
{
    QFile f{autostart_dir.filePath(autostart_filename)};
    ASSERT_TRUE(f.exists());
    ASSERT_TRUE(f.open(QIODevice::ReadOnly | QIODevice::Text));

    auto actual_contents = QString{f.readAll()};
    EXPECT_EQ(actual_contents, autostart_contents);
}

struct PlatformLinux : public mpt::TestWithMockedBinPath
{
    template <typename VMFactoryType>
    void aux_test_driver_factory(const QString& driver = QStringLiteral(""))
    {
        setup_driver_settings(driver);

        decltype(mp::platform::vm_backend("")) factory_ptr;
        EXPECT_NO_THROW(factory_ptr = mp::platform::vm_backend(backend_path););

        EXPECT_TRUE(dynamic_cast<VMFactoryType*>(factory_ptr.get()));
    }

    void with_minimally_mocked_libvirt(std::function<void()> test_contents)
    {
        mp::LibvirtWrapper libvirt_wrapper{""};
        test_contents();
    }

    mpt::UnsetEnvScope unset_env_scope{mp::driver_env_var};
    mpt::SetEnvScope disable_apparmor{"DISABLE_APPARMOR", "1"};
};

TEST_F(PlatformLinux, test_interpretation_of_winterm_setting_not_supported)
{
    for (const auto x : {"no", "matter", "what"})
        EXPECT_THROW(mp::platform::interpret_setting(mp::winterm_key, x), mp::InvalidSettingsException);
}

TEST_F(PlatformLinux, test_interpretation_of_unknown_settings_not_supported)
{
    for (const auto k : {"unimaginable", "katxama", "katxatxa"})
        for (const auto v : {"no", "matter", "what"})
            EXPECT_THROW(mp::platform::interpret_setting(k, v), mp::InvalidSettingsException);
}

TEST_F(PlatformLinux, test_empty_sync_winterm_profiles)
{
    EXPECT_NO_THROW(mp::platform::sync_winterm_profiles());
}

TEST_F(PlatformLinux, test_autostart_desktop_file_properly_placed)
{
    try
    {
        const auto& [autostart_dir, autostart_filename, autostart_contents, guards] =
            setup_autostart_desktop_file_test();

        ASSERT_FALSE(HasFailure()) << "autostart test setup failed";

        mp::platform::setup_gui_autostart_prerequisites();
        check_autostart_file(autostart_dir, autostart_filename, autostart_contents);
    }
    catch (autostart_test_setup_error& e)
    {
        FAIL() << e.what();
    }
}

TEST_F(PlatformLinux, test_autostart_setup_replaces_wrong_link)
{
    try
    {
        const auto& [autostart_dir, autostart_filename, autostart_contents, guards] =
            setup_autostart_desktop_file_test();

        ASSERT_FALSE(HasFailure()) << "autostart test setup failed";

        const auto bad_filename = autostart_dir.filePath("wrong_file");
        QFile bad_file{bad_filename};

        EXPECT_TRUE(bad_file.open(QIODevice::WriteOnly)); // create desktop file to link against
        bad_file.write("bad contents");
        bad_file.close();

        bad_file.link(autostart_dir.filePath(autostart_filename)); // link to a bad file

        mp::platform::setup_gui_autostart_prerequisites();
        check_autostart_file(autostart_dir, autostart_filename, autostart_contents);
    }
    catch (autostart_test_setup_error& e)
    {
        FAIL() << e.what();
    }
}

TEST_F(PlatformLinux, test_autostart_setup_replaces_broken_link)
{
    try
    {
        const auto& [autostart_dir, autostart_filename, autostart_contents, guards] =
            setup_autostart_desktop_file_test();

        ASSERT_FALSE(HasFailure()) << "autostart test setup failed";

        const auto bad_filename = autostart_dir.filePath("absent_file");
        QFile bad_file{bad_filename};
        ASSERT_FALSE(bad_file.exists());
        bad_file.link(autostart_dir.filePath(autostart_filename)); // link to absent file
        ASSERT_FALSE(bad_file.exists());

        mp::platform::setup_gui_autostart_prerequisites();
        check_autostart_file(autostart_dir, autostart_filename, autostart_contents);
    }
    catch (autostart_test_setup_error& e)
    {
        FAIL() << e.what();
    }
}

TEST_F(PlatformLinux, test_autostart_setup_leaves_non_link_file_alone)
{
    try
    {
        const auto& [autostart_dir, autostart_filename, autostart_contents, guards] =
            setup_autostart_desktop_file_test();

        ASSERT_FALSE(HasFailure()) << "autostart test setup failed";

        const auto replacement_contents = "replacement contents";

        {
            QFile replacement_file{autostart_dir.filePath(autostart_filename)};
            ASSERT_FALSE(replacement_file.exists());
            ASSERT_TRUE(replacement_file.open(QIODevice::WriteOnly)); // create desktop file to link against
            ASSERT_EQ(replacement_file.write(replacement_contents), qstrlen(replacement_contents));
        }

        mp::platform::setup_gui_autostart_prerequisites();
        check_autostart_file(autostart_dir, autostart_filename, replacement_contents);
    }
    catch (autostart_test_setup_error& e)
    {
        FAIL() << e.what();
    }
}

TEST_F(PlatformLinux, test_autostart_setup_fails_on_absent_desktop_target)
{
    const auto guard_xdg = temporarily_change_env("XDG_DATA_DIRS", "/dadgad/bad/dir");
    const auto guard_home = temporarily_change_env("HOME", "dadgbd/bad/too");

    EXPECT_THROW(mp::platform::setup_gui_autostart_prerequisites(), mp::AutostartSetupException);
}

TEST_F(PlatformLinux, test_default_qemu_driver_produces_correct_factory)
{
    auto factory = mpt::MockProcessFactory::Inject();
    aux_test_driver_factory<mp::QemuVirtualMachineFactory>();
}

TEST_F(PlatformLinux, test_explicit_qemu_driver_produces_correct_factory)
{
    auto factory = mpt::MockProcessFactory::Inject();
    aux_test_driver_factory<mp::QemuVirtualMachineFactory>("qemu");
}

TEST_F(PlatformLinux, test_libvirt_driver_produces_correct_factory)
{
    auto test = [this] { aux_test_driver_factory<mp::LibVirtVirtualMachineFactory>("libvirt"); };
    with_minimally_mocked_libvirt(test);
}

TEST_F(PlatformLinux, test_lxd_driver_produces_correct_factory)
{
    aux_test_driver_factory<mp::LXDVirtualMachineFactory>("lxd");
}

TEST_F(PlatformLinux, test_qemu_in_env_var_is_ignored)
{
    mpt::SetEnvScope env(mp::driver_env_var, "QEMU");
    auto test = [this] { aux_test_driver_factory<mp::LibVirtVirtualMachineFactory>("libvirt"); };
    with_minimally_mocked_libvirt(test);
}

TEST_F(PlatformLinux, test_libvirt_in_env_var_is_ignored)
{
    auto factory = mpt::MockProcessFactory::Inject();
    mpt::SetEnvScope env(mp::driver_env_var, "LIBVIRT");
    aux_test_driver_factory<mp::QemuVirtualMachineFactory>("qemu");
}

TEST_F(PlatformLinux, workflowsURLOverrideSetReturnsExpectedData)
{
    const QString fake_url{"https://a.fake.url"};
    mpt::SetEnvScope workflows_url("MULTIPASS_WORKFLOWS_URL", fake_url.toUtf8());

    EXPECT_EQ(MP_PLATFORM.get_workflows_url_override(), fake_url);
}

TEST_F(PlatformLinux, workflowsURLOverrideNotSetReturnsEmptyString)
{
    EXPECT_TRUE(MP_PLATFORM.get_workflows_url_override().isEmpty());
}

TEST_F(PlatformLinux, test_is_remote_supported_returns_true)
{
    EXPECT_TRUE(MP_PLATFORM.is_remote_supported("release"));
    EXPECT_TRUE(MP_PLATFORM.is_remote_supported("daily"));
    EXPECT_TRUE(MP_PLATFORM.is_remote_supported(""));
    EXPECT_TRUE(MP_PLATFORM.is_remote_supported("snapcraft"));
    EXPECT_TRUE(MP_PLATFORM.is_remote_supported("appliance"));
}

TEST_F(PlatformLinux, test_is_remote_supported_lxd)
{
    setup_driver_settings("lxd");

    EXPECT_TRUE(MP_PLATFORM.is_remote_supported("release"));
    EXPECT_TRUE(MP_PLATFORM.is_remote_supported("daily"));
    EXPECT_TRUE(MP_PLATFORM.is_remote_supported(""));
    EXPECT_TRUE(MP_PLATFORM.is_remote_supported("appliance"));
    EXPECT_FALSE(MP_PLATFORM.is_remote_supported("snapcraft"));
}

TEST_F(PlatformLinux, test_snap_returns_expected_default_address)
{
    const QByteArray base_dir{"/tmp"};
    const QByteArray snap_name{"multipass"};

    mpt::SetEnvScope env("SNAP_COMMON", base_dir);
    mpt::SetEnvScope env2("SNAP_NAME", snap_name);

    EXPECT_EQ(mp::platform::default_server_address(), fmt::format("unix:{}/multipass_socket", base_dir.toStdString()));
}

TEST_F(PlatformLinux, test_not_snap_returns_expected_default_address)
{
    const QByteArray snap_name{"multipass"};

    mpt::UnsetEnvScope unset_env("SNAP_COMMON");
    mpt::SetEnvScope env2("SNAP_NAME", snap_name);

    EXPECT_EQ(mp::platform::default_server_address(), fmt::format("unix:/run/multipass_socket"));
}

TEST_F(PlatformLinux, test_is_alias_supported_returns_true)
{
    EXPECT_TRUE(MP_PLATFORM.is_alias_supported("focal", "release"));
}

struct TestUnsupportedDrivers : public TestWithParam<QString>
{
};

TEST_P(TestUnsupportedDrivers, test_unsupported_driver)
{
    ASSERT_FALSE(mp::platform::is_backend_supported(GetParam()));

    setup_driver_settings(GetParam());
    EXPECT_THROW(mp::platform::vm_backend(backend_path), std::runtime_error);
}

INSTANTIATE_TEST_SUITE_P(PlatformLinux, TestUnsupportedDrivers,
                         Values(QStringLiteral("hyperkit"), QStringLiteral("hyper-v"), QStringLiteral("other")));

TEST_F(PlatformLinux, retrieves_empty_bridges)
{
    const mpt::TempDir tmp_dir;
    const auto fake_bridge = "somebridge";

    QDir fake_sys_class_net{tmp_dir.path()};
    QDir bridge_dir{fake_sys_class_net.filePath(fake_bridge)};
    ASSERT_EQ(mpt::make_file_with_content(bridge_dir.filePath("type"), "1"), 1);
    ASSERT_TRUE(bridge_dir.mkpath("bridge"));

    auto net_map = mp::platform::detail::get_network_interfaces_from(fake_sys_class_net.path());

    using value_type = decltype(net_map)::value_type;
    using Net = mp::NetworkInterfaceInfo;
    EXPECT_THAT(net_map, ElementsAre(AllOf(
                             Field(&value_type::first, fake_bridge),
                             Field(&value_type::second, AllOf(Field(&Net::id, fake_bridge), Field(&Net::type, "bridge"),
                                                              Field(&Net::description, StrEq("Network bridge")))))));
}

TEST_F(PlatformLinux, retrieves_ethernet_devices)
{
    const mpt::TempDir tmp_dir;
    const auto fake_eth = "someth";

    QDir fake_sys_class_net{tmp_dir.path()};
    ASSERT_EQ(mpt::make_file_with_content(fake_sys_class_net.filePath(fake_eth) + "/type", "1"), 1);

    auto net_map = mp::platform::detail::get_network_interfaces_from(fake_sys_class_net.path());

    ASSERT_EQ(net_map.size(), 1u);

    auto it = net_map.cbegin();
    EXPECT_EQ(it->first, fake_eth);
    EXPECT_EQ(it->second.id, fake_eth);
    EXPECT_EQ(it->second.type, "ethernet");
    EXPECT_EQ(it->second.description, "Ethernet device");
}

TEST_F(PlatformLinux, does_not_retrieve_unknown_networks)
{
    const mpt::TempDir tmp_dir;
    const auto fake_nets = {"eth0", "foo", "kkkkk"};

    QDir fake_sys_class_net{tmp_dir.path()};
    for (const auto& net : fake_nets)
        ASSERT_TRUE(fake_sys_class_net.mkpath(net));

    EXPECT_THAT(mp::platform::detail::get_network_interfaces_from(fake_sys_class_net.path()), IsEmpty());
}

TEST_F(PlatformLinux, does_not_retrieve_other_virtual)
{
    const mpt::TempDir tmp_dir;
    const auto fake_virt = "somevirt";

    QDir fake_sys_class_net{tmp_dir.path() + "/virtual"};
    ASSERT_EQ(mpt::make_file_with_content(fake_sys_class_net.filePath(fake_virt) + "/type", "1"), 1);

    EXPECT_THAT(mp::platform::detail::get_network_interfaces_from(fake_sys_class_net.path()), IsEmpty());
}

TEST_F(PlatformLinux, does_not_retrieve_wireless)
{
    const mpt::TempDir tmp_dir;
    const auto fake_wifi = "somewifi";

    QDir fake_sys_class_net{tmp_dir.path()};
    QDir wifi_dir{fake_sys_class_net.filePath(fake_wifi)};
    ASSERT_EQ(mpt::make_file_with_content(wifi_dir.filePath("type"), "1"), 1);
    ASSERT_TRUE(wifi_dir.mkpath("wireless"));

    EXPECT_THAT(mp::platform::detail::get_network_interfaces_from(fake_sys_class_net.path()), IsEmpty());
}

TEST_F(PlatformLinux, does_not_retrieve_protocols)
{
    const mpt::TempDir tmp_dir;
    const auto fake_net = "somenet";

    QDir fake_sys_class_net{tmp_dir.path()};
    ASSERT_EQ(mpt::make_file_with_content(fake_sys_class_net.filePath(fake_net) + "/type", "32"), 2);

    EXPECT_THAT(mp::platform::detail::get_network_interfaces_from(fake_sys_class_net.path()), IsEmpty());
}

TEST_F(PlatformLinux, does_not_retrieve_other_specified_device_types)
{
    const mpt::TempDir tmp_dir;
    const auto fake_net = "somenet";
    const auto uevent_contents = std::string{"asdf\nDEVTYPE=crazytype\nfdsa"};

    QDir fake_sys_class_net{tmp_dir.path()};
    QDir net_dir{fake_sys_class_net.filePath(fake_net)};
    ASSERT_EQ(mpt::make_file_with_content(net_dir.filePath("type"), "1"), 1);
    ASSERT_EQ(mpt::make_file_with_content(net_dir.filePath("uevent"), uevent_contents),
              static_cast<int64_t>(uevent_contents.size()));

    auto net_map = mp::platform::detail::get_network_interfaces_from(fake_sys_class_net.path());

    EXPECT_THAT(mp::platform::detail::get_network_interfaces_from(fake_sys_class_net.path()), IsEmpty());
}

struct BridgeMemberTest : public PlatformLinux, WithParamInterface<std::vector<std::pair<std::string, bool>>>
{
};

TEST_P(BridgeMemberTest, retrieves_bridges_with_members)
{
    const mpt::TempDir tmp_dir;
    const auto fake_bridge = "aeiou";

    QDir fake_sys_class_net{tmp_dir.path()};
    QDir interface_dir{fake_sys_class_net.filePath(fake_bridge)};
    QDir members_dir{interface_dir.filePath("brif")};

    ASSERT_EQ(mpt::make_file_with_content(interface_dir.filePath("type"), "1"), 1);
    ASSERT_TRUE(interface_dir.mkpath("bridge"));
    ASSERT_TRUE(members_dir.mkpath("."));

    using net_value_type = decltype(MP_PLATFORM.get_network_interfaces_info())::value_type;
    std::vector<Matcher<net_value_type>> network_matchers{};
    std::vector<Matcher<std::string>> substrs_matchers{};
    for (const auto& [member, recognized] : GetParam())
    {
        QDir member_dir = fake_sys_class_net.filePath(QString::fromStdString(member));
        ASSERT_TRUE(member_dir.mkpath("."));
        ASSERT_TRUE(members_dir.mkpath(QString::fromStdString(member)));

        if (recognized)
        {
            ASSERT_EQ(mpt::make_file_with_content(member_dir.filePath("type"), "1"), 1);

            substrs_matchers.push_back(HasSubstr(member));
            network_matchers.push_back(Field(&net_value_type::first, member));
        }
        else
            substrs_matchers.push_back(Not(HasSubstr(member)));
    }

    auto net_map = mp::platform::detail::get_network_interfaces_from(fake_sys_class_net.path());

    using Net = mp::NetworkInterfaceInfo;
    network_matchers.push_back(
        AllOf(Field(&net_value_type::first, fake_bridge),
              Field(&net_value_type::second, AllOf(Field(&Net::id, fake_bridge), Field(&Net::type, "bridge"),
                                                   Field(&Net::description, AllOfArray(substrs_matchers))))));

    EXPECT_THAT(net_map, UnorderedElementsAreArray(network_matchers));
}

using Param = std::vector<std::pair<std::string, bool>>;
INSTANTIATE_TEST_SUITE_P(
    PlatformLinux, BridgeMemberTest,
    Values(Param{{"en0", true}}, Param{{"en0", false}}, Param{{"en0", false}, {"en1", true}},
           Param{{"asdf", true}, {"ggi", true}, {"a1", true}, {"fu", false}, {"ho", true}, {"ra", false}}));

} // namespace
