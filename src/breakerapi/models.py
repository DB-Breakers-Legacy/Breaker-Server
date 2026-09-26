from django.db import models


class PatrollerUser(models.Model):
    userID = models.PositiveBigIntegerField(primary_key=True)
    gameCurrency = models.JSONField()
    messageList = models.JSONField()
    rivalList = models.JSONField()
    userCharacters = models.JSONField()


class MessageBot(models.Model):
    messageID = models.CharField(max_length=32, primary_key=True)  # example: 20260313_CosMatch
    messageContents = models.JSONField()
