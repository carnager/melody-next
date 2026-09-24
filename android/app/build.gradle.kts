import java.io.File
import java.util.Properties

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
    id("org.jetbrains.kotlin.plugin.compose")
}

// Release signing, from gradle properties, the environment or a key file
// outside the repository -- never from anything checked in.
val releaseSigningProperties = Properties()
val releaseSigningPropertiesFile = file(
    System.getenv("MELODY_ANDROID_SIGNING_PROPERTIES")
        ?: "${System.getProperty("user.home")}/.local/android/release-keys/melody.properties"
)
if (releaseSigningPropertiesFile.isFile) {
    releaseSigningPropertiesFile.inputStream().use(releaseSigningProperties::load)
}

fun signingValue(name: String): String? =
    providers.gradleProperty(name).orNull
        ?: System.getenv(name)
        ?: releaseSigningProperties.getProperty(name)

fun signingFile(path: String): File =
    if (path.startsWith("~/")) File(System.getProperty("user.home"), path.removePrefix("~/"))
    else file(path)

android {
    namespace = "com.melody.next"
    compileSdk = 36
    buildToolsVersion = "37.0.0"

    defaultConfig {
        // Its own id, so it installs beside the MPD-era app while both exist.
        applicationId = "com.melody.next"
        minSdk = 26
        targetSdk = 35
        versionCode = 1
        versionName = "0.1.0"
    }

    signingConfigs {
        create("release") {
            val storePath = signingValue("MELODY_ANDROID_STORE_FILE")
            if (storePath != null) {
                storeFile = signingFile(storePath)
                storePassword = signingValue("MELODY_ANDROID_STORE_PASSWORD")
                keyAlias = signingValue("MELODY_ANDROID_KEY_ALIAS")
                keyPassword = signingValue("MELODY_ANDROID_KEY_PASSWORD")
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = true
            if (signingValue("MELODY_ANDROID_STORE_FILE") != null) {
                signingConfig = signingConfigs.getByName("release")
            }
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }
    buildFeatures {
        compose = true
    }
    testOptions {
        unitTests.isReturnDefaultValues = true
    }
}

dependencies {
    val composeBom = platform("androidx.compose:compose-bom:2024.12.01")
    implementation(composeBom)
    implementation("androidx.compose.material3:material3")
    implementation("androidx.compose.ui:ui")
    implementation("androidx.compose.ui:ui-tooling-preview")
    implementation("androidx.compose.material:material-icons-extended")
    implementation("androidx.activity:activity-compose:1.9.3")
    implementation("androidx.lifecycle:lifecycle-viewmodel-compose:2.8.7")
    implementation("androidx.lifecycle:lifecycle-runtime-compose:2.8.7")
    implementation("androidx.lifecycle:lifecycle-process:2.8.7")
    // The notification and lock-screen controls: a media session over a
    // player that is the engine, not this phone.
    implementation("androidx.media3:media3-session:1.6.1")
    implementation("androidx.media3:media3-common:1.6.1")
    // The phone as a speaker: what the engine sends, played here.
    implementation("androidx.media3:media3-exoplayer:1.6.1")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.9.0")
    debugImplementation("androidx.compose.ui:ui-tooling")

    testImplementation("junit:junit:4.13.2")
    testImplementation("org.json:json:20231013")
    testImplementation("org.jetbrains.kotlinx:kotlinx-coroutines-test:1.9.0")
}
