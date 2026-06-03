# Face Detection (ESPHome external component)

Détection (et reconnaissance) de visage pour ESP32-P4 basée sur ESP-DL.

## Configuration de base

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/youkorr/face_detection
    components: [face_detection]

face_detection:
  id: face_det
  camera_id: my_camera
  model_type: face_recognition
  score_threshold: 0.3
  detection_interval: 8
  draw_enabled: true
```

## Switch d'activation / désactivation

Une plateforme `switch` permet d'activer ou de désactiver la détection à la
volée (depuis Home Assistant, un bouton, une automatisation, etc.). Quand le
switch est sur `OFF`, le pipeline de détection est suspendu : aucune inférence
n'est exécutée et plus aucun cadre n'est dessiné. Quand il repasse sur `ON`,
la détection reprend immédiatement.

```yaml
switch:
  - platform: face_detection
    name: "Détection de visage"
    face_detection_id: face_det   # optionnel si un seul composant face_detection
```

Options héritées de la plateforme `switch` ESPHome :

| Option           | Défaut                | Description                                            |
| ---------------- | --------------------- | ------------------------------------------------------ |
| `name`           | —                     | Nom de l'entité exposée à Home Assistant.              |
| `restore_mode`   | `RESTORE_DEFAULT_ON`  | État restauré au démarrage (activé par défaut).        |
| `icon`           | `mdi:face-recognition`| Icône de l'entité.                                     |

Exemple : couper la détection automatiquement la nuit.

```yaml
switch:
  - platform: face_detection
    id: face_switch
    name: "Détection de visage"

time:
  - platform: homeassistant
    on_time:
      - hours: 23
        then:
          - switch.turn_off: face_switch
      - hours: 7
        then:
          - switch.turn_on: face_switch
```
