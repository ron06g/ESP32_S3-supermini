#inclure « WiFi.h »
 
configuration annulaire()
{
Serial.start(115200);
 
// Réglez le WiFi en mode station et déconnectez-vous d'un AP s'il était précédemment connecté
Mode WiFi (WIFI_STA) ;
WiFi.déconnexion();
délai (100);
 
Serial.println("Configuration réalisée) ;
}
 
boucle vide()
{
Serial.println(""scan start");
 
// WiFi.scanRéseaux renverra le nombre de réseaux trouvés
int n = WiFi.scanRéseaux();
Serial.println(""scan fait");
si (n == 0) {
Serial.println("aucun réseau trouvé");
} autre {
Série.print(n) ;
Serial.println("" réseaux trouvés");
pour (int i = 0 ; i < n ; ++i) {
// Imprimez SSID et RSSI pour chaque réseau trouvé
Serial.print(i + 1);
Série.print(": ");
Serial.print (WiFi.SSID(i));
Série.print("" (");
Serial.print (WiFi.RSSI(i) ;
Serial.print(")");
Serial.println((WiFi.encryptionType(i) == WIFI_AUTH_OPEN)?"":"*");
délai (10);
}
}
Série.println("");
 
// Attendez un peu avant de reanner
délai (5 000) ;
}