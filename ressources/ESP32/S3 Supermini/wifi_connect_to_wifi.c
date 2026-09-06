#inclure <WiFi.h>
 
const char* ssid = "votre-ssid"; //votre nom WiFi
const char* mot de passe = « votre mot de passe » ; //votre mot de passe WiFi
 
configuration annulaire()
{
Serial.start(115200);
délai (10);
 
// Nous commençons par vous connecter à un réseau WiFi
 
Série.println();
Série.println();
Serial.print("Connexion à ");
Série.println(ssid);
 
WiFi.start(cssid, mot de passe) ;
 
tandis (WiFi.status()!= WL_CONNECTED) {
retard (500);
Serial.print(".");
}
 
Série.println("");
Serial.println("WiFi connecté");
Serial.println("Adresse IP : ");
Serial.println(WiFi.localIP());
}
boucle vide()
{
}