#inclure « WiFi.h »

configuration annulaire()

{

Serial.start(115200);

WiFi.softAP("ESP_AP", "123456789");

}



boucle vide()

{

Serial.print("Nom de l'hôte:");

Serial.println(WiFi.softAPgetHostname());

Serial.print("Host IP:");

Serial.println(WiFi.softAPIP());

Serial.print("Host IPV6:");

Serial.println(WiFi.softAPIPv6());

Serial.print("Host SSID:");

Serial.println(WiFi.SSID());

Serial.print (« IP de diffusion hôte : » ;

Serial.println(WiFi.softAPBroadcastIP());

Serial.print("Host mac Adresse :)" ;

Serial.println(WiFi.softAPmacAddress());

Serial.print (nombre de connexions hôtes : »);

Serial.println(WiFi.softAPgetStationNum());

Serial.print (« ID réseau hôte : ») ;

Serial.println(WiFi.softAPNetworkID());

Serial.print (""Statut de l'hôte :)" ;

Serial.println(WiFi.status());

délai (1 000) ;

}