#pragma once

namespace wattlink {
namespace page {

// All the texts of the setup page are in this file, so the language can be switched here and nowhere else.
constexpr const char *kTitle = "Pengaturan WATTLINK Hub";
constexpr const char *kIntro = "Ketik nama dan kata sandi Wi-Fi toko Anda agar Hub bisa terhubung ke internet.";
constexpr const char *kBandHint = "Hub hanya bisa memakai Wi-Fi 2,4 GHz. Wi-Fi 5 GHz tidak didukung.";
constexpr const char *kSsidLabel = "Nama Wi-Fi (SSID)";
constexpr const char *kPasswordLabel = "Kata sandi Wi-Fi";
constexpr const char *kPasswordHint = "Kosongkan hanya jika Wi-Fi toko tidak memakai kata sandi.";
constexpr const char *kSaveButton = "Simpan";

constexpr const char *kSsidEmpty = "Nama Wi-Fi tidak boleh kosong.";
constexpr const char *kSsidTooLong = "Nama Wi-Fi terlalu panjang. Maksimal 32 karakter, dan huruf beraksen atau emoji dihitung lebih dari satu karakter.";
constexpr const char *kPasswordTooShort = "Kata sandi minimal 8 karakter.";
constexpr const char *kPasswordTooLong = "Kata sandi maksimal 63 karakter.";
constexpr const char *kBadCharacters =
    "Ada karakter yang tidak boleh dipakai. Nama Wi-Fi tidak boleh berisi karakter yang tidak terlihat, dan kata sandi hanya boleh "
    "berisi huruf tanpa aksen, angka, spasi, dan simbol biasa.";
constexpr const char *kConfirmOpen =
    "Wi-Fi tanpa kata sandi tidak terenkripsi, jadi lalu lintas Wi-Fi Hub bisa dibaca orang di dekatnya. "
    "Tekan Simpan sekali lagi jika Anda yakin.";
constexpr const char *kSaveFailed = "Gagal menyimpan. Coba lagi.";
constexpr const char *kExpired = "Sesi pengaturan sudah berakhir. Tekan tombol Hub selama 3 detik untuk memulai lagi.";

constexpr const char *kSavedTitle = "Tersimpan";
constexpr const char *kSavedBody = "Hub sedang menyambung ke Wi-Fi Anda. Jaringan pengaturan ini akan ditutup. Anda bisa menutup halaman ini.";

}  // namespace page
}  // namespace wattlink
