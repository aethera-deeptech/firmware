#pragma once

namespace wattlink {
namespace page {

// All the texts of the setup page are in this file, so the language can be switched here and nowhere else.
constexpr const char *kTitle = "Pengaturan WATTLINK Hub";
constexpr const char *kIntro = "Pilih Wi-Fi toko Anda agar Hub bisa terhubung ke internet.";
constexpr const char *kSsidLabel = "Nama Wi-Fi (SSID)";
constexpr const char *kPasswordLabel = "Kata sandi Wi-Fi";
constexpr const char *kPasswordHint = "Kosongkan hanya jika Wi-Fi toko tidak memakai kata sandi.";
constexpr const char *kSaveButton = "Simpan";

constexpr const char *kSsidEmpty = "Nama Wi-Fi tidak boleh kosong.";
constexpr const char *kSsidTooLong = "Nama Wi-Fi terlalu panjang. Maksimal 32 byte.";
constexpr const char *kPasswordTooShort = "Kata sandi minimal 8 karakter.";
constexpr const char *kPasswordTooLong = "Kata sandi maksimal 63 karakter.";
constexpr const char *kBadCharacters =
    "Nama Wi-Fi tidak boleh berisi karakter kontrol. Kata sandi hanya boleh berisi huruf, angka, spasi, dan simbol ASCII.";
constexpr const char *kConfirmOpen =
    "Wi-Fi tanpa kata sandi tidak terenkripsi, jadi data antara Hub dan router bisa dibaca orang di dekatnya. "
    "Tekan Simpan sekali lagi jika Anda yakin.";
constexpr const char *kSaveFailed = "Gagal menyimpan. Coba lagi.";
constexpr const char *kExpired = "Sesi pengaturan sudah berakhir. Tekan tombol Hub selama 3 detik untuk memulai lagi.";

constexpr const char *kSavedTitle = "Tersimpan";
constexpr const char *kSavedBody = "Hub sedang menyambung ke Wi-Fi Anda. Jaringan pengaturan ini akan ditutup. Anda bisa menutup halaman ini.";

}  // namespace page
}  // namespace wattlink
