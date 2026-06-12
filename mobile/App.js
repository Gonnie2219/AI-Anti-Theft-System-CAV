import { useEffect, useRef, useState, useCallback } from 'react';
import {
  ActivityIndicator,
  Pressable,
  RefreshControl,
  ScrollView,
  StyleSheet,
  Text,
  TextInput,
  View,
} from 'react-native';
import { SafeAreaProvider, SafeAreaView } from 'react-native-safe-area-context';
import { StatusBar } from 'expo-status-bar';
import { WebView } from 'react-native-webview';
import * as SecureStore from 'expo-secure-store';

const DASHBOARD_URL = 'https://webapp-seven-livid-86.vercel.app';
const STORE_KEY_USER = 'dashboard_username';
const STORE_KEY_PASS = 'dashboard_password';

// Fills the dashboard's own login form and fires its existing submit handler,
// so CMD_SECRET and all downstream logic are derived by the page's code.
// Reports the outcome back via postMessage. Credentials are JSON-escaped —
// never logged, never placed in a URL.
function buildAutoLoginScript(username, password) {
  return `
(function() {
  var USER = ${JSON.stringify(username)};
  var PASS = ${JSON.stringify(password)};
  function notify(msg) {
    if (window.ReactNativeWebView) window.ReactNativeWebView.postMessage(msg);
  }
  function attempt(triesLeft) {
    var u = document.getElementById('username');
    var p = document.getElementById('password');
    var f = document.getElementById('login-form');
    var login = document.getElementById('login');
    if (!u || !p || !f) {
      if (triesLeft > 0) return setTimeout(function() { attempt(triesLeft - 1); }, 300);
      return notify('auth-error');
    }
    if (login && login.style.display === 'none') return notify('auth-ok'); // already unlocked
    u.value = USER;
    p.value = PASS;
    f.dispatchEvent(new Event('submit', { bubbles: true, cancelable: true }));
    // The page's handler is async (crypto.subtle) — poll for the outcome.
    var waited = 0;
    var timer = setInterval(function() {
      waited += 250;
      if (login.style.display === 'none') { clearInterval(timer); notify('auth-ok'); }
      else if (waited >= 4000) { clearInterval(timer); notify('auth-failed'); }
    }, 250);
  }
  attempt(10);
})();
true;`;
}

export default function App() {
  const webViewRef = useRef(null);
  const [storeChecked, setStoreChecked] = useState(false);
  const [credentials, setCredentials] = useState(null); // {username, password} | null
  const [loginError, setLoginError] = useState('');
  const [usernameInput, setUsernameInput] = useState('');
  const [passwordInput, setPasswordInput] = useState('');
  const [loading, setLoading] = useState(true);
  const [refreshing, setRefreshing] = useState(false);
  const [refreshEnabled, setRefreshEnabled] = useState(true);

  // Load saved credentials on launch
  useEffect(() => {
    (async () => {
      try {
        const [user, pass] = await Promise.all([
          SecureStore.getItemAsync(STORE_KEY_USER),
          SecureStore.getItemAsync(STORE_KEY_PASS),
        ]);
        if (user && pass) setCredentials({ username: user, password: pass });
      } catch {
        // Corrupt/unavailable store — fall through to the login screen
      }
      setStoreChecked(true);
    })();
  }, []);

  const handleSave = useCallback(async () => {
    const user = usernameInput.trim();
    const pass = passwordInput;
    if (!user || !pass) {
      setLoginError('Enter username and password.');
      return;
    }
    await SecureStore.setItemAsync(STORE_KEY_USER, user);
    await SecureStore.setItemAsync(STORE_KEY_PASS, pass);
    setLoginError('');
    setPasswordInput('');
    setLoading(true);
    setCredentials({ username: user, password: pass });
  }, [usernameInput, passwordInput]);

  const handleLogout = useCallback(async () => {
    await SecureStore.deleteItemAsync(STORE_KEY_USER);
    await SecureStore.deleteItemAsync(STORE_KEY_PASS);
    setCredentials(null);
    setUsernameInput('');
    setPasswordInput('');
    setLoginError('');
  }, []);

  const onMessage = useCallback((event) => {
    const msg = event.nativeEvent.data;
    if (msg === 'auth-failed') {
      // Stored credentials rejected (wrong or rotated password):
      // return to the native screen so the user can overwrite them.
      setCredentials(null);
      setLoginError('Dashboard rejected the saved login. Please re-enter.');
    }
  }, []);

  const onRefresh = useCallback(() => {
    setRefreshing(true);
    webViewRef.current?.reload();
  }, []);

  if (!storeChecked) {
    return (
      <SafeAreaProvider>
        <View style={styles.loadingOverlay}>
          <ActivityIndicator size="large" color="#0ea5e9" />
        </View>
      </SafeAreaProvider>
    );
  }

  // --- Native login screen ---
  if (!credentials) {
    return (
      <SafeAreaProvider>
        <SafeAreaView style={styles.safeArea}>
          <StatusBar style="light" />
          <View style={styles.loginContainer}>
            <Text style={styles.title}>Anti-Theft System</Text>
            <TextInput
              style={styles.input}
              placeholder="Username"
              placeholderTextColor="#64748b"
              autoCapitalize="none"
              autoCorrect={false}
              value={usernameInput}
              onChangeText={setUsernameInput}
            />
            <TextInput
              style={styles.input}
              placeholder="Password"
              placeholderTextColor="#64748b"
              secureTextEntry
              autoCapitalize="none"
              value={passwordInput}
              onChangeText={setPasswordInput}
              onSubmitEditing={handleSave}
            />
            <Pressable style={styles.button} onPress={handleSave}>
              <Text style={styles.buttonText}>Save & continue</Text>
            </Pressable>
            {loginError ? <Text style={styles.error}>{loginError}</Text> : null}
          </View>
        </SafeAreaView>
      </SafeAreaProvider>
    );
  }

  // --- Dashboard WebView ---
  return (
    <SafeAreaProvider>
      <SafeAreaView style={styles.safeArea} edges={['top', 'left', 'right']}>
        <StatusBar style="light" />
        <View style={styles.header}>
          <Text style={styles.headerTitle}>Anti-Theft Dashboard</Text>
          <Pressable onPress={handleLogout} hitSlop={10}>
            <Text style={styles.logout}>Log out</Text>
          </Pressable>
        </View>
        <ScrollView
          style={styles.flex}
          contentContainerStyle={styles.flex}
          refreshControl={
            <RefreshControl
              refreshing={refreshing}
              onRefresh={onRefresh}
              enabled={refreshEnabled}
              tintColor="#0ea5e9"
            />
          }
        >
          <WebView
            ref={webViewRef}
            source={{ uri: DASHBOARD_URL }}
            style={styles.flex}
            injectedJavaScript={buildAutoLoginScript(
              credentials.username,
              credentials.password
            )}
            onMessage={onMessage}
            onLoadEnd={() => {
              setLoading(false);
              setRefreshing(false);
            }}
            onScroll={(e) =>
              setRefreshEnabled(e.nativeEvent.contentOffset.y <= 0)
            }
            // Camera / geolocation support for dashboard requests
            geolocationEnabled
            mediaCapturePermissionGrantType="grant"
            allowsInlineMediaPlayback
            mediaPlaybackRequiresUserAction={false}
            // Misc
            javaScriptEnabled
            domStorageEnabled
            setSupportMultipleWindows={false}
            startInLoadingState
            renderLoading={() => null}
          />
          {loading && (
            <View style={styles.loadingOverlay} pointerEvents="none">
              <ActivityIndicator size="large" color="#0ea5e9" />
            </View>
          )}
        </ScrollView>
      </SafeAreaView>
    </SafeAreaProvider>
  );
}

const styles = StyleSheet.create({
  flex: { flex: 1 },
  safeArea: { flex: 1, backgroundColor: '#0b1220' },
  loadingOverlay: {
    ...StyleSheet.absoluteFillObject,
    alignItems: 'center',
    justifyContent: 'center',
    backgroundColor: '#0b1220',
  },
  loginContainer: {
    flex: 1,
    justifyContent: 'center',
    paddingHorizontal: 32,
  },
  title: {
    color: '#e2e8f0',
    fontSize: 22,
    fontWeight: '700',
    textAlign: 'center',
    marginBottom: 24,
  },
  input: {
    backgroundColor: '#1e293b',
    borderColor: '#334155',
    borderWidth: 1,
    borderRadius: 8,
    color: '#e2e8f0',
    paddingHorizontal: 14,
    paddingVertical: 12,
    marginBottom: 12,
    fontSize: 16,
  },
  button: {
    backgroundColor: '#0ea5e9',
    borderRadius: 8,
    paddingVertical: 14,
    alignItems: 'center',
    marginTop: 4,
  },
  buttonText: { color: '#fff', fontSize: 16, fontWeight: '600' },
  error: { color: '#f87171', textAlign: 'center', marginTop: 12 },
  header: {
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'space-between',
    paddingHorizontal: 14,
    paddingVertical: 8,
    backgroundColor: '#0b1220',
    borderBottomWidth: 1,
    borderBottomColor: '#1e293b',
  },
  headerTitle: { color: '#e2e8f0', fontSize: 14, fontWeight: '600' },
  logout: { color: '#0ea5e9', fontSize: 14, fontWeight: '600' },
});
